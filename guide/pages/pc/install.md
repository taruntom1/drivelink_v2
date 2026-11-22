# Install the app

You need a 64-bit PC running Windows 11 or Ubuntu 26.04 and an internet connection. At the end you’ll have DriveLink installed and open to **Connect**. You can install it before plugging in the board.

<div class="command-tabs" data-choice-group>
<div data-choice="Windows">

1. {{windows}}. Find the setup file in Downloads. You can [verify its checksum]({{base}}downloads/#verify-a-download) before opening it.
2. Open the setup file and follow the installer. You should see **DriveLink** in the Start menu when setup finishes.
3. Open **DriveLink**. The window should say **Connect to a device**.

</div>
<div data-choice="Ubuntu">

Use one installation method below. Both install the app and its dependencies.

### Install from the signed apt repository

1. Follow [Install from apt]({{base}}apt/#add-the-repository). You should see apt read the DriveLink source without a signature error.
2. Run the command below. Apt should list `drivelink` and finish setting it up.

   ~~~sh
   sudo apt install drivelink
   ~~~

### Install from downloaded packages

1. Open a terminal and check your computer’s architecture. The result should be `amd64` or `arm64`.

   ~~~sh
   dpkg --print-architecture
   ~~~

2. Download both files in the matching row into an empty folder. You should have two `.deb` files for the same release. You can [verify their checksums]({{base}}downloads/#verify-a-download).

   | Architecture | Desktop app | Required dependency |
   | --- | --- | --- |
   | amd64 | {{gui_amd64}} | {{cli_amd64}} |
   | arm64 | {{gui_arm64}} | {{cli_arm64}} |

3. Open a terminal in that folder and run the command below. Apt should install both files along with any dependencies from Ubuntu.

   ~~~sh
   sudo apt install ./drivelink_*.deb ./drivelink-cli_*.deb
   ~~~

After either install, search your application launcher for **DriveLink** and open it. The window should say **Connect to a device**.

For more on apt and packages, see [Ubuntu’s package management guide](https://ubuntu.com/server/docs/how-to/software/package-management/).

</div>
</div>

For a new board, [flash DriveLink]({{base}}flash/). If DriveLink is already on the board, [connect and tour the app]({{base}}pc/connect/).
