# Buku Panduan (Manual Book) - Gembot AI Companion

## 1. Pendahuluan
**Gembot** adalah robot pendamping cerdas (AI Companion) berukuran mini yang ditenagai oleh kecerdasan buatan (Gemini LLM) untuk interaksi suara, fitur text-to-speech (TTS), dan memiliki antarmuka wajah bergaya *pixel-art* di layarnya.

Gembot dirancang untuk berinteraksi dengan pengguna secara natural (menggunakan mikrofon dan speaker), memonitor kondisi lingkungan sekitar (suhu dan gerakan), serta memiliki *Dashboard* kontrol interaktif via web browser.

---

## 2. Spesifikasi Perangkat Keras (Hardware)
Sistem Gembot menggunakan mikrokontroler **ESP32-S3** sebagai otak utamanya di sisi hardware. Berikut adalah daftar komponen dan diagram koneksi pin (Wiring):

1. **Layar TFT ILI9341 (Menampilkan Wajah & Menu)**
   - Jalur: SPI
   - Pin: MISO (13), MOSI (11), SCLK (12), CS (10), DC (9), RST (14)
2. **Mikrofon INMP441 (Merekam Suara Pengguna)**
   - Jalur: I2S In
   - Pin: SCK (4), WS (5), SD (6)
3. **Speaker Amplifier MAX98357A (Mengeluarkan Suara AI)**
   - Jalur: I2S Out
   - Pin: BCLK (15), LRC (16), DOUT (17)
4. **Modul Pemutar Musik DFPlayer Mini (Memutar MP3 Lokal dari SD Card)**
   - Jalur: UART
   - Pin: TX (21), RX (18)
5. **Sensor Gyro/Accelerometer MPU6050 (Mendeteksi Gerakan/Guncangan)**
   - Jalur: I2C
   - Pin: SDA (1), SCL (2)
6. **Sensor Suhu & Kelembaban DHT22**
   - Jalur: GPIO
   - Pin: Data (8)
7. **Sensor Sentuh (Touch Sensor)**
   - Jalur: GPIO
   - Pin: Data (7)
8. **Modul Kelistrikan (Tidak Terhubung Langsung ke Pin Data)**
   - **Baterai Li-Po / Li-Ion** dengan pembacaan voltase di Pin ADC (3).
   - **Modul TP4056** untuk manajemen pengisian baterai (Charging).
   - **Modul Step-up MT3608** untuk menaikkan tegangan ke 5V.
   - **Saklar Daya (Switch)** untuk On/Off.

---

## 3. Sistem Perangkat Lunak (Software)
Gembot beroperasi menggunakan sistem *Client-Server*:

- **Client (Robot ESP32)**
  Diprogram menggunakan **C++ (PlatformIO / Arduino Framework)**. File utamanya adalah `gembot2.cpp`. Bertugas membaca input sensor, merekam suara, menggambar animasi ke layar TFT, dan mengirim/menerima data dari server melalui koneksi WiFi.

- **Server Utama (Node.js)**
  Diprogram menggunakan **JavaScript (Node.js)**. File utamanya adalah `web_serial_server.js`. Server ini berfungsi sebagai jembatan antara Gembot dan layanan eksternal (Google Gemini API). Server ini menangani konversi percakapan (LLM) dan menghasilkan suara sintesis (TTS), yang kemudian dialirkan kembali ke Gembot.

- **Database (SQLite)**
  History percakapan antara pengguna dan Gembot disimpan di dalam database lokal (SQLite) pada server.

---

## 4. Cara Penggunaan (Panduan Operasional)

### 4.1 Menghidupkan Gembot
1. Nyalakan saklar (Switch) daya pada Gembot.
2. Gembot akan memulai proses *booting* dan otomatis terhubung ke WiFi yang telah dikonfigurasi.
3. Pastikan server Node.js sudah berjalan di komputer/VPS (jalankan perintah `node web_serial_server.js` atau `pm2 start web_serial_server.js`).
4. Saat Gembot tersambung ke server, layar akan menampilkan ekspresi normal (Senyum).

### 4.2 Mengakses Web Dashboard (Control Panel)
1. Buka browser di perangkat Anda (laptop atau smartphone).
2. Akses Web Dashboard Gembot publik melalui tautan berikut:
   **`https://212-2-253-247.sslip.io/control`**
   *(Atau gunakan `http://localhost:3001/control` jika server dijalankan secara lokal)*
3. Dari Dashboard ini, pengguna bisa:
   - Melihat status baterai, suhu, kelembaban, dan tingkat gerakan secara *real-time*.
   - Mengubah ekspresi wajah Gembot secara manual.
   - Memutar, menjeda, atau mengganti lagu pada DFPlayer.
   - Mengatur alarm/pengingat.
   - Mengontrol volume suara robot.

### 4.3 Berbicara dengan Gembot
1. Gembot memiliki mikrofon INMP441. Saat robot dalam status mendengarkan, bicaralah dengan jelas ke arah mikrofon.
2. Suara Anda akan dikirim ke server, diproses oleh AI Gemini, lalu AI akan memberikan teks balasan.
3. Server akan mengubah teks balasan menjadi suara (TTS), lalu Gembot akan memutarnya melalui speaker MAX98357A.
4. Ekspresi wajah Gembot akan merespons sesuai dengan percakapan (misalnya marah, senang, sedih).

---

## 5. Batasan dan Catatan Penting
- **Ketergantungan Internet & Server**: Gembot tidak bisa memproses suara dan merespons pembicaraan jika tidak terhubung ke server atau tidak ada akses internet.
- **Limit Harian AI**: Layanan AI memiliki kuota yang diatur pada server (saat ini diset maksimal **300 percakapan/hari**). Jika batas ini tercapai, Gembot akan berhenti merespons hingga sistem di-reset.
- **Modul Kelistrikan**: Modul cas (TP4056) dan step-up (MT3608) adalah perangkat *hardware* murni dan tidak bisa diprogram via *coding*. Jika baterai habis, isi daya layaknya powerbank biasa.
