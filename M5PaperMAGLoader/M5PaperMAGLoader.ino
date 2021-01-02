// MAG表示ロジックは、https://emk.name/2015/03/magjs.html で公開されているロジックを移植したものです。

#include <M5EPD.h>

M5EPD_Canvas canvas(&M5.EPD);

#include <esp_heap_caps.h>

__attribute__ ((always_inline)) inline static
uint16_t swap565( uint8_t r, uint8_t g, uint8_t b) {
  return ((b >> 3) << 8) | ((g >> 2) << 13) | ((g >> 5) | ((r >> 3) << 3));
}

#define MAG_DIRECTORY "/mag"

void setup(void) {
  M5.begin();
  M5.EPD.SetRotation(0);
  M5.EPD.Clear(true);
  M5.EPD.SetColorReverse(true);
  //overlay bg on full screen
  canvas.createCanvas(960, 540);
}

void loop() {
  randomDraw();
}

void randomDraw() {
  File magRoot;
  magRoot = SD.open(MAG_DIRECTORY);
  int fileCount = 0;
  while (1) {
    File entry =  magRoot.openNextFile();
    if (!entry) { // no more files
      break;
    }
    canvas.fillCanvas(G0);
    canvas.setTextColor(G15);

    //ファイルのみ取得
    if (!entry.isDirectory()) {
      String fileName = entry.name();
      fileName.toUpperCase();
      if (fileName.endsWith("MAG") == true) {
        canvas.setTextSize(3);
        canvas.drawString(entry.name(), 45, 500);
        magLoad(entry);
        delay(2000);
      }
    }
    entry.close();
  }
  magRoot.close();
}

void magLoad(File dataFile) {
  uint8_t maki02[8];
  dataFile.read(maki02, 8);
  if (0 != memcmp (maki02, "MAKI02  ", 8)) {
    //MAG画像ではない
    canvas.drawString(" is Not MAG Format.", 400, 500);
    return;
  }

  //ヘッダ先頭まで読み捨て
  int headerOffset = 8;
  dataFile.seek(headerOffset, SeekSet);
  while (dataFile.available() && 0x1A != dataFile.read()) ++headerOffset;

  headerOffset++;

  dataFile.seek( headerOffset, SeekSet);

  struct mag_info_t {
    // 先頭32バイト分はファイル内の順序通り。順序変更不可。
    uint8_t top;
    uint8_t machine;
    uint8_t flags;
    uint8_t mode;
    uint16_t sx;
    uint16_t sy;
    uint16_t ex;
    uint16_t ey;
    uint32_t flagAOffset;
    uint32_t flagBOffset;
    uint32_t flagBSize;
    uint32_t pixelOffset;
    uint32_t pixelSize;

    uint32_t flagASize;
    uint16_t colors;
    uint8_t pixelUnitLog;
    uint16_t width;
    uint16_t height;
    uint16_t flagSize;

    void init(void) {
      flagASize = flagBOffset - flagAOffset;

      colors = mode & 0x80 ? 256 : 16;
      pixelUnitLog = mode & 0x80 ? 1 : 2;
      width = (ex | 7) - (sx & 0xFFF8) + 1;
      height = ey - sy + 1;
      flagSize = width >> (pixelUnitLog + 1);
    }
  } __attribute__((packed)); // 1バイト境界にアライメントを設定

  mag_info_t mag;

  dataFile.read((uint8_t*)&mag, 32);
  mag.init();

  // 16ライン分の画像データ展開領域
  uint8_t *data = (uint8_t *)heap_caps_malloc(mag.width * 16, MALLOC_CAP_8BIT);

  // カラーパレット展開領域
  uint8_t *palette = (uint8_t *)heap_caps_malloc(mag.colors * 3, MALLOC_CAP_8BIT);
  dataFile.read(palette, mag.colors * 3);

  uint8_t *flagABuf = (uint8_t *)heap_caps_malloc(mag.flagASize, MALLOC_CAP_8BIT);
  dataFile.seek( headerOffset + mag.flagAOffset, SeekSet);
  dataFile.read(flagABuf, mag.flagASize);

  uint8_t *flagBBuf = (uint8_t *)heap_caps_malloc(mag.flagBSize, MALLOC_CAP_8BIT);
  dataFile.seek( headerOffset + mag.flagBOffset, SeekSet);
  dataFile.read(flagBBuf, mag.flagBSize);

  uint8_t *flagBuf = (uint8_t *)heap_caps_malloc(mag.flagSize, MALLOC_CAP_8BIT);
  memset(flagBuf, 0, mag.flagSize);

  static constexpr int pixel_bufsize = 4096;
  static constexpr auto halfbufsize = pixel_bufsize >> 1;
  uint8_t *pixel = (uint8_t *)heap_caps_malloc(pixel_bufsize, MALLOC_CAP_8BIT);
  dataFile.seek( headerOffset + mag.pixelOffset, SeekSet);

  uint32_t src = 0; // (headerOffset + mag.pixelOffset) % (pixel_bufsize);
  dataFile.read(&pixel[src], pixel_bufsize - src);

  uint_fast16_t flagAPos = 0;
  uint_fast16_t flagBPos = 0;
  int32_t dest = 0;
  // コピー位置の計算
  static constexpr uint8_t copyx[] = {0, 1, 2, 4, 0, 1, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0};
  static constexpr uint8_t copyy[] = {0, 0, 0, 0, 1, 1, 2, 2, 2, 4, 4, 4, 8, 8, 8, 16};
  int32_t copypos[16];

  for (int i = 0; i < 16; ++i) {
    copypos[i] = -(copyy[i] * mag.width + (copyx[i] << mag.pixelUnitLog));
  }

  int copysize = 1 << mag.pixelUnitLog;
  uint_fast8_t mask = 0x80;

  //  uint16_t *linebuf= (uint16_t *)heap_caps_malloc(320, MALLOC_CAP_DMA);
  //  uint16_t linebuf[640];

  Serial.printf("width=%d:height=%d\n", mag.width, mag.height);

  int32_t destdiff = 0;

  int wid = mag.width;
  if (wid > 640) wid = 640;
  //canvas.setAddrWindow(0, 0, wid, 480);
  int height = mag.height;
  if (height > 480) height = 480;
  for (int y = 0; y < height; ++y) {
    int dy = (y - 1) & 15;
    int x = 0;
    for (; x < wid; x++) {
      if ((x) + (dy) * mag.width > dest) break;
      int c1 = data[ x      +  dy      * mag.width] * 3;
      int color =
        swap565(
          (palette[c1 + 1]) >> 2,
          (palette[c1    ]) >> 2,
          (palette[c1 + 2]) >> 2
        ) / 4096;
      canvas.drawPixel(((960 - wid) / 2) + x, y + 10, color);
    }
    if (y + 1 == height) break;
    if ((y & 15) == 0) {
      destdiff = dest;
      dest = 0;
    }

    // フラグを1ライン分展開
    //int x = 0;
    x = 0;
    int xend = mag.flagSize;
    do {
      // フラグAを1ビット調べる
      if (flagABuf[flagAPos] & mask) {
        // 1ならフラグBから1バイト読んでXORを取る
        flagBuf[x] ^= flagBBuf[flagBPos++];
      }
      if ((mask >>= 1) == 0) {
        mask = 0x80;
        ++flagAPos;
      }
    } while (++x < xend);

    x = 0;
    xend <<= 1;
    do {
      // フラグを1つ調べる
      uint_fast8_t v = flagBuf[x >> 1];
      if (x & 1) v &= 0x0F;
      else v >>= 4;

      if (!v) {
        if (src == pixel_bufsize) {
          dataFile.read(pixel, pixel_bufsize);
          src = 0;
        }
        // 0ならピクセルデータから1ピクセル(2バイト)読む
        if (mag.colors == 16) {
          auto tmp = pixel[src]; // 一時変数を使う事で効率向上
          data[dest    ] = tmp >> 4;
          data[dest + 1] = tmp & 0xF;
          tmp = pixel[src + 1];
          data[dest + 2] = tmp >> 4;
          data[dest + 3] = tmp & 0xF;
          dest += 4;
          src += 2;
        } else {
          memcpy(&data[dest], &pixel[src], 2);
          dest += 2;
          src += 2;
        }
      } else {
        // 0以外なら指定位置から1ピクセル(16色なら4ドット/256色なら2ドット)コピー
        int32_t copySrc = dest + copypos[v];
        if (copySrc < 0) copySrc += destdiff;
        memcpy(&data[dest], &data[copySrc], copysize);
        dest += copysize;
      }
    } while (++x < xend);
  }
  M5.EPD.Clear(true);
  canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

  heap_caps_free(palette);
  heap_caps_free(flagABuf);
  heap_caps_free(flagBBuf);
  heap_caps_free(flagBuf);
  heap_caps_free(pixel);
  heap_caps_free(data);
}
