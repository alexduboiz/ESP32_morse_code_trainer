# ESP32 Morse Trainer

ESP32 Morse Trainer is an ESP-IDF application for decoding Morse code from a dual-paddle keyer and displaying live decoded text in a browser.

## Features

- Dual-paddle keyer support (dot and dash inputs)
- Real-time Morse decoding to plain text
- Browser-based live UI served from the ESP32
- WebSocket updates for decoded text, symbol buffer, and speed changes
- Clear button and speed slider in the web interface
- Status LED indicates an active symbol

## Morse Code Reference

The trainer decodes standard Morse code symbols. Use the following characters as a reference:

- A: .-          N: -.
- B: -...        O: ---
- C: -.-.        P: .--.
- D: -..         Q: --.-
- E: .           R: .-.
- F: ..-.        S: ...
- G: --.         T: -
- H: ....        U: ..-
- I: ..          V: ...-
- J: .---        W: .--
- K: -.-         X: -..-
- L: .-..        Y: -.--
- M: --          Z: --..

Numbers:

- 0: -----
- 1: .----
- 2: ..---
- 3: ...--
- 4: ....-
- 5: .....
- 6: -....
- 7: --...
- 8: ---..
- 9: -----

Common punctuation:

- Period (`.`): .-.-.-
- Comma (`,`): --..--
- Question mark (`?`): ..--..
- Slash (`/`): -..-.
- Parentheses (`(` `)`): -.--.-
- Ampersand (`&`): .-...
- Colon (`:`): ---...
- Semicolon (`;`): -.-.-.
- Equals (`=`): -...-
- Plus (`+`): .-.-.
- Hyphen (`-`): -....-
- Underscore (`_`): ..--.-
- Quotation mark (`"`): .-..-.
- At sign (`@`): .--.-.

## Hardware

This project is configured for the following GPIO pins:

- `DIT_PIN` = GPIO 4
- `DAH_PIN` = GPIO 5
- `RGB_PIN` = GPIO 48

The paddle inputs are configured as input pins with pull-up resistors, so the switches should pull the line to ground when pressed.

## Software Requirements

- ESP-IDF 5.4.1 or compatible version
- Windows environment with ESP-IDF tools initialized

## Build Instructions

1. Open a terminal in the project folder:

   ```powershell
   cd C:\Users\alexd\esp32_morse_trainer
   ```

2. Initialize ESP-IDF in the current shell if needed.

   In PowerShell, use the call operator `&` to execute the script in the current session:
   ```powershell
   & 'C:\Users\alexd\esp\v5.4.1\esp-idf\export.ps1'
   ```

   If you are using Command Prompt, use:
   ```cmd
   call C:\Users\alexd\esp\v5.4.1\esp-idf\export.bat
   ```

   > Note: In PowerShell, running the batch file directly may not keep the environment changes.

3. Configure the WiFi credentials:

   ```powershell
   idf.py menuconfig
   ```

   Navigate to:

   - `Component config`
   - `main`
   - `WiFi Configuration`

   If you do not see the menu, press `/` and search for `WiFi` or `SSID`.

   Set `WiFi SSID` and `WiFi Password` there.

4. Build the project:

   ```powershell
   idf.py build
   ```

5. Flash the firmware to the ESP32:

   ```powershell
   idf.py -p <PORT> flash
   ```

5. Monitor serial output:

   ```powershell
   idf.py -p <PORT> monitor
   ```

## Usage

- After startup, the board connects to the configured WiFi network.
- Open a browser and navigate to the ESP32 IP address.
- The page connects over WebSocket to receive live decoder updates.
- Use the slider to adjust dot timing and the Clear button to reset the decoded text.

## Configuration

WiFi credentials are managed via project configuration.

Run `idf.py menuconfig` and set `WiFi SSID` and `WiFi Password` under the `WiFi Configuration` menu.

For production use, consider secure storage instead of storing credentials in `sdkconfig`.

### Troubleshooting

If `idf.py` is not found after activating ESP-IDF:

- In PowerShell, run:
  ```powershell
  & 'C:\Users\alexd\esp\v5.4.1\esp-idf\export.ps1'
  ```
- Do not execute the script by typing its path as a string.
- If you use PowerShell, avoid running `export.bat` directly, because its environment changes may not persist in the PowerShell session.

## Project Structure

- `CMakeLists.txt` - Root CMake project file
- `main/CMakeLists.txt` - Component registration and dependencies
- `main/main.cpp` - Application source code
- `idf-component.yml` - Component manifest

## Notes

- The project uses native ESP-IDF APIs for WiFi, HTTP server, and GPIO.
- WebSocket support is implemented manually over BSD sockets.
- The current implementation allows a single WebSocket client.

## License

Add your preferred license here before publishing.

