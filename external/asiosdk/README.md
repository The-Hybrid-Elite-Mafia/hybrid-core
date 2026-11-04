# Steinberg ASIO SDK — drop zone (never committed)

The ASIO SDK is Steinberg's code, offered under GPLv3 or Steinberg's proprietary licence.
It is kept out of this repo; `.gitignore` blocks everything here except this file.

Fetch it (done automatically by the GitHub build):
```
powershell -ExecutionPolicy Bypass -File tools\fetch-asio-sdk.ps1
```
That downloads ASIO SDK 2.3.4 from download.steinberg.net, checks its SHA-512, applies the
asiolist.cpp delete[] fix and installs it to `external/asiosdk/sdk/`. Then configure with
`-DHC_WITH_ASIO=ON`.
