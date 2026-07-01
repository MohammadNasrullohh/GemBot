# Tabel Panduan Penyambungan (Wiring) Gembot ESP32-S3

Tabel ini berfungsi sebagai panduan penyolderan kabel dari modul ke pin ESP32-S3. Seluruh jalur VCC menggunakan tegangan **3.3V (3V3)**.

### 1. Modul Layar (TFT ILI9341)
| Pin TFT ILI9341 | Sambung ke Pin ESP32-S3 |
| :--- | :--- |
| **VCC** | **3V3** |
| **GND** | **GND** |
| **CS** | **10** |
| **RESET** | **14** |
| **DC** (A0/RS) | **9** |
| **SDI** (MOSI) | **11** |
| **SCK** (SCLK) | **12** |
| **SDO** (MISO) | **13** |
| **LED** (Backlight)| **3V3** |

### 2. Mikrofon (INMP441 - I2S)
| Pin INMP441 | Sambung ke Pin ESP32-S3 |
| :--- | :--- |
| **VDD** | **3V3** |
| **GND** | **GND** |
| **L/R** | **GND** |
| **SCK** | **4** |
| **WS** | **5** |
| **SD** | **6** |

### 3. Speaker / Audio Amp (MAX98357A - I2S)
| Pin MAX98357A | Sambung ke Pin ESP32-S3 |
| :--- | :--- |
| **VIN** | **3V3** |
| **GND** | **GND** |
| **BCLK** | **15** |
| **LRC** | **16** |
| **DIN** | **17** |
| **SD_MODE** | *(Tidak disolder / Kosong)* |

### 4. Modul Pemutar MP3 (DFPlayer Mini)
| Pin DFPlayer | Sambung ke Pin ESP32-S3 |
| :--- | :--- |
| **VCC** | **3V3** |
| **GND** | **GND** |
| **RX** | **21** |
| **TX** | **18** |
| **SPK_1** | **Speaker 2 (+)** |
| **SPK_2** | **Speaker 2 (-)** |
| **Kapasitor (Kaki Panjang/+)** | **VCC** DFPlayer |
| **Kapasitor (Kaki Pendek/-)** | **GND** DFPlayer |

### 5. Sensor Suhu (DHT22)
| Pin DHT22 | Sambung ke Pin ESP32-S3 |
| :--- | :--- |
| **VCC** (+) | **3V3** |
| **GND** (-) | **GND** |
| **DATA** (Out)| **8** |

### 6. Sensor Gyro (MPU6050)
| Pin MPU6050 | Sambung ke Pin ESP32-S3 |
| :--- | :--- |
| **VCC** | **3V3** |
| **GND** | **GND** |
| **SCL** | **2** |
| **SDA** | **1** |

### 7. Sensor Sentuh (TTP223 / Touch Sensor)
| Pin TTP223 | Sambung ke Pin ESP32-S3 |
| :--- | :--- |
| **VCC** | **3V3** |
| **GND** | **GND** |
| **I/O** (SIG) | **7** |


---
**Perhatian:** Pastikan seluruh pin **GND** dari semua modul terhubung bersama menjadi satu jalur menuju pin **GND** pada ESP32. Seluruh modul di atas dihubungkan ke tegangan **3.3V (3V3)**. Kapasitor pada DFPlayer dipasang memotong (paralel) antara pin VCC dan GND DFPlayer untuk menyaring *noise* suara.
