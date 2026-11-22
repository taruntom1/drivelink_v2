# Downloads

Find the app for your computer and the firmware that runs on the ESP32-S3. Check your operating system and architecture before you choose a download.

{{release_label}}

## Desktop app

| Your computer | Download |
| --- | --- |
| Windows 11, 64-bit Intel or AMD | {{windows}} |
| Ubuntu 26.04, 64-bit Intel or AMD (amd64) | {{gui_amd64}} |
| Ubuntu 26.04, 64-bit ARM (arm64) | {{gui_arm64}} |

On Windows, run the installer. You should see **DriveLink** in the Start menu when setup finishes. On Ubuntu, [install from apt]({{base}}apt/) to get the app and its dependencies. The Debian packages (.deb files) above are also available for manual installation.

## ESP32-S3 firmware

{{firmware}} contains the firmware and instructions for first flash. Use [Flash in the browser]({{base}}flash/) to install it on a new board or reset an existing one.

## Linux and ROS 2 packages

[Install from apt]({{base}}apt/) for the DriveLink agent (the service on the Pi) and the ROS 2 packages. Apt verifies the packages against the signed repository. The [release page]({{release_url}}) lists the individual package files.

## Verify a download

1. {{checksums}} and open that file in a text editor. Find the line for your download. GitHub displays some Linux package filenames with a dot before ubuntu. The checksum list uses the original filename, with a tilde.
2. Open a terminal in your Downloads folder and compute the SHA-256 checksum, a string that identifies the exact file contents. Replace FILE with the downloaded filename.

<div class="command-tabs" data-choice-group>
<div data-choice="Windows">

~~~powershell
Get-FileHash -Algorithm SHA256 'FILE'
~~~

</div>
<div data-choice="Ubuntu">

~~~sh
sha256sum 'FILE'
~~~

</div>
</div>

3. Compare the 64 characters you see with the ones on the matching line in SHA256SUMS. Letter case can differ. If any character differs, discard that download and download it again.
