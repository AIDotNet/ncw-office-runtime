# LibreOfficeKit headers (vendored)

These five headers are copied verbatim from the LibreOffice source tree:

- Source: https://github.com/LibreOffice/core/tree/libreoffice-26-8/include/LibreOfficeKit
- Branch: `libreoffice-26-8` (matches the LibreOffice 26.8 runtime the helper was verified against)
- License: Mozilla Public License 2.0 (see the header of each file; https://mozilla.org/MPL/2.0/)

They are not modified. The helper only uses them to call into a LibreOffice
installation at runtime (`lok_init`); no LibreOffice code is compiled into the helper.

Distributing a LibreOffice runtime alongside the helper carries its own license
obligations (MPL-2.0 / LGPL-3.0-or-later / other third-party components). Those must be
reviewed for the exact build that gets shipped; this directory does not settle them.
