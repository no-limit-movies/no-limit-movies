# ⚡ No Limit Movies

Super-lightweight **Windows C++** app that turns your PC into a fully automated media machine.  
Powered by **Jackett**, **qBittorrent**, and **MPV**, it searches, downloads, and plays torrents automatically.  
Control it from your phone via **HTTP Shortcuts** — search → download → watch, 100% hands-free.  

> **Designed for legitimate torrent use only** — such as public-domain content or your own media.

> ⚠ **Setup Required**:  
> This project will not run without editing the source code to add your own Jackett, qBittorrent, and MPV credentials in Visual Studio, then compiling it yourself.

> **Legal Notice**  
> This software is intended for legitimate torrent use only — such as public-domain films, Creative Commons works, or your own personal content. The author is not responsible for any misuse.

---

## ✨ Features
- **Fully automated workflow**: search → download → play without manual steps
- **Jackett integration** for flexible torrent searching
- **qBittorrent control** for download management
- **MPV player automation** with IPC commands
- **HTTP Shortcuts remote control** from your phone
- **Watched list** to track viewed content

---

## 📦 Requirements
- **Windows 10/11**
- **C++23** compatible compiler (MSVC recommended)
- [Jackett](https://github.com/Jackett/Jackett)
- [qBittorrent](https://www.qbittorrent.org/)
- [MPV](https://mpv.io/)

---

## 🔧 Installation

1. **Download the source code**  
   - Click the green **Code** button on this page → **Download ZIP**  
   - Or clone with Git:
     ```bash
     git clone https://github.com/no-limit-movies/no-limit-movies.git
     cd no-limit-movies
     ```

2. **Open in Visual Studio**  
   - Launch **Visual Studio 2022** (or newer) with C++ development tools installed  
   - Open the `No Limit Movies - Public.sln` solution file

3. **Add your credentials**  
   - In the source code, find the section with Jackett, qBittorrent, and MPV settings  
   - Replace the placeholders with your actual details:  
     - **Jackett** → URL + API key  
     - **qBittorrent** → host, port, username, password  
     - **MPV** → executable path and IPC pipe name  
   - Save the changes

4. **Build the project**  
   - In Visual Studio: **Build → Build Solution** (`Ctrl+Shift+B`)  
   - Output executable will be in the `x64\Release` folder (or `x64\Debug` if building in Debug mode)

5. **Run**  
   - Double-click the compiled `NoLimitMovies.exe`  
   - Ensure Jackett, qBittorrent, and MPV are installed and running

---

## 💡 Project Roadmap
No Limit Movies is currently an open-source project licensed under AGPL v3.0 to encourage contributions and collaboration.  
Once the codebase is stable, a Pro edition and commercial licensing options may be offered, while keeping the core version free and open source.

---

## 🤝 Contributing
Pull requests are welcome!  
If you’d like to contribute:
1. Fork the repository  
2. Create a feature branch (`git checkout -b feature-name`)  
3. Commit your changes  
4. Push to your branch and open a Pull Request

---

## 📜 License
Licensed under **GNU AGPL v3.0** — see the [`LICENSE`](LICENSE) file for details.  
Future versions may offer a **Pro edition** under a commercial license.



   
