# PicoCalc FTP Server

A simple FTP server for Raspberry Pi Pico 2 W integrated into a PicoCalc, allowing file transfers to and from an SD card.

## Features

- **Complete FTP server** on Raspberry Pi Pico 2 W
- **SD card storage** for files
- **ILI9488 display** (320x320) for status information
- **Simple configuration** via a text file on the SD card
- **Automatic WiFi connection**
- **Custom FTP ports** for better compatibility (21 for commands, 21000 for passive data)

## Hardware Requirements

- Raspberry Pi Pico 2W (Pico W not tested)
- SD card
- PicoCalc

## Connection Diagram

### SD Card Connection

| Pico W | SD Card |
|--------|---------|
| GPIO 18 (SCK) | SCK |
| GPIO 19 (MOSI) | MOSI |
| GPIO 16 (MISO) | MISO |
| GPIO 17 (CS) | CS |
| GPIO 22 | SD_DETECT (optional) |

### ILI9488 Display Connection

| Pico W | ILI9488 Display |
|--------|----------------|
| GPIO 10 (SPI1 SCK) | SCK |
| GPIO 11 (SPI1 MOSI) | MOSI |
| GPIO 12 (SPI1 MISO) | MISO |
| GPIO 13 | CS |
| GPIO 14 | DC |
| GPIO 15 | RST |

## Installation

1. Clone this repository:
   ```bash
   git clone https://github.com/yourusername/PicoCalc_FTP.git
   cd PicoCalc_FTP
   ```

2. Install PlatformIO on VSCode.

3. Compile and upload the code:
   ```bash
   platformio run -e raspberypi_pico2w -t upload
   ```

## Configuration

1. Create a `config_ftp.conf` file at the root of your SD card with the following content:
   ```
   ssid=Your_WiFi_SSID
   password=Your_WiFi_Password
   ftp_user=FTP_Username
   ftp_password=FTP_Password
   ```

2. Insert the SD card into the reader connected to the Pico W

3. Power on the system and wait for the screen to display the IP address and FTP connection information

## Usage

1. Connect to the FTP server using an FTP client (FileZilla, WinSCP, etc.)
   - Host: IP address displayed on the screen
   - Port: 21
   - Username: The one configured in `config_ftp.conf`
   - Password: The one configured in `config_ftp.conf`
   - Mode: Active (if passive mode doesn't work)

2. You can now transfer files to and from the SD card

## Supported FTP Commands

- `USER` / `PASS`: Authentication
- `LIST`: File listing
- `PWD` / `CWD`: Directory management
- `RETR` / `STOR`: Download/Upload files
- `DELE`: Delete files
- `MKD` / `RMD`: Create/Delete directories
- `RNFR` / `RNTO`: Rename files
- `SYST`: System information
- And other standard FTP commands

## Troubleshooting

- **WiFi connection issues**: Check the SSID and password in the configuration file
- **FTP connection issues**: 
  - Make sure your FTP client is configured to use port 21
  - Try active mode if passive mode doesn't work (!)
- **SD card issues**: Check that the SD card is properly formatted (FAT32) and that the configuration file is present

## Development

This project uses the following libraries:
- [SimpleFTPServer](https://github.com/xreef/SimpleFTPServer) for the FTP server
- [LovyanGFX](https://github.com/lovyan03/LovyanGFX) for display management
- [SdFat](https://github.com/greiman/SdFat) for SD card management

## Contribution

Contributions are welcome! Feel free to open an issue or submit a pull request.

## Author

Kiwil

## Acknowledgments

- [Renzo Mischianti](https://github.com/xreef) for the SimpleFTPServer library
- [Lovyan03](https://github.com/lovyan03) for the LovyanGFX library
- [Bill Greiman](https://github.com/greiman) for the SdFat library
