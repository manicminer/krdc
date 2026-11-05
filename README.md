# KRDC - KDE Remote Desktop Client

<p align="center">
  <img src="logo.png" alt="KRDC Logo" width="200">
</p>

KRDC is a remote desktop client for the KDE Plasma Desktop environment. It supports protocols like RDP, VNC, and SPICE, allowing users to connect to remote machines seamlessly. KRDC provides a user-friendly interface for accessing remote desktops, making it a powerful tool for remote administration or accessing your own machines from afar.

For more detailed information, including user guides and technical documentation, check out the official KDE resources:
- [KRDC Overview on KDE Apps](https://apps.kde.org/krdc/)
- [KRDC User Documentation](https://docs.kde.org/stable5/en/krdc/krdc/index.html)

---

## About this fork

This is an **unofficial fork** of KDE's KRDC, not supported by KDE or Microsoft. It adds experimental **Azure Virtual Desktop (AVD)** support: Microsoft Entra sign-in in your browser, the AVD workspace list, and connecting to full desktops. See [doc/azure-virtual-desktop.md](doc/azure-virtual-desktop.md) for details and limitations.

---

## Features

- **Multi-Protocol Support:** Connect effortlessly using industry-standard protocols, including **RDP** (Remote Desktop Protocol), **VNC** (Virtual Network Computing), and **SPICE**.
- **KDE Integration:** Seamlessly integrates with the KDE Plasma Desktop environment, providing a consistent and user-friendly experience.
- **Session Management:** Easily manage and revisit your remote connections with session history and quick access to recent connections.
- **Enhanced Security:** Secure your remote sessions with configurable encryption options, ensuring your data remains protected.
- **Azure Virtual Desktop (experimental):** Sign in with Microsoft Entra ID in your browser, browse your AVD workspaces and connect to full desktops. RemoteApps are listed but cannot be launched yet.

---

## Installation

### Flatpak (recommended)

This fork is published as `io.github.manicminer.krdc` (shown as **KRDC AVD**) in the shared [manicminer Flatpak repository](https://manicminer.github.io/flatpak/). The `.flatpakref` also sets up the runtime it needs:

```bash
flatpak install --user https://manicminer.github.io/flatpak/io.github.manicminer.krdc.flatpakref
```

To update:

```bash
flatpak update --user io.github.manicminer.krdc
```

See [packaging/flatpak/README.md](packaging/flatpak/README.md) for other ways to install and for the sandbox design.

### Building from Source

Requires Qt 6, KDE Frameworks 6, extra-cmake-modules, QtKeychain, and FreeRDP and FreeRDP-Client 3.32.2 or later. Microsoft Entra sign-in and AVD need FreeRDP built with `WITH_AAD=ON`. CMake reports any other missing dependencies; see [doc/azure-virtual-desktop.md](doc/azure-virtual-desktop.md#building) for build options and the offline tests.

```bash
git clone https://github.com/manicminer/krdc.git
cd krdc
cmake -S . -B build
cmake --build build
sudo cmake --install build
```

---

## Usage

1. **Launch KRDC:**
   ```bash
   flatpak run io.github.manicminer.krdc   # Flatpak
   krdc                                    # built from source
   ```

2. **Connect to a remote machine:**
   - Enter the remote host address (e.g., `rdp://192.168.1.10`, `vnc://hostname`, or `spice://myhost:5900`).
   - Choose the connection protocol (RDP, VNC, or SPICE).
   - Click **Connect**.

3. **Session Management:**
   - KRDC keeps a history of recent connections for quick access.
   - You can bookmark favorite connections for easy management.

4. **Azure Virtual Desktop (experimental):**
   - Choose **File ▸ Azure Virtual Desktop Workspaces…** and click **Sign In…**.
   - Click **Open Sign-in Page in Browser**, sign in, and allow the browser to open the `ms-appx-web:` link; the dialog closes on its own. When built from source, first use **Set Up KRDC for Sign-in Links…** once.
   - Under **Devices**, double-click a desktop (or select it and click **Connect**). RemoteApps appear under **Apps** but cannot be launched yet.

---

## Contributing

We welcome contributions from the community! Here's how you can help:

1. **Fork the repository** and clone it locally.
2. Create a new branch for your feature or bugfix.
3. **Submit a merge request (MR)** with a clear description of your changes.

For detailed contribution guidelines, please refer to [KDE's Contribution Guide](https://community.kde.org/Get_Involved/development).

---

## License

KRDC is distributed under the **GNU General Public License (GPL) 2.0 or later**. This covers most of the KRDC source code. See [LICENSES/GPL-2.0-or-later.txt](LICENSES/GPL-2.0-or-later.txt) for details.

For more information, please review each license file in the repository.

---

## Community & Support

- [KRDC Project Page](https://invent.kde.org/network/krdc)
- [KDE Community Forum](https://forum.kde.org/)
- [KDE Bug Tracker](https://bugs.kde.org/)

## Azure Virtual Desktop and .rdp files (experimental)

See [doc/azure-virtual-desktop.md](doc/azure-virtual-desktop.md) for importing `.rdp` files, Microsoft Entra sign-in, the AVD workspace list, limitations and the offline tests.
