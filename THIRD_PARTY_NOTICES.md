# Third-party notices

## pdfminer.six CCITT tables

The standard CCITT T.4/T.6 code trees in `pdg_converter.py`,
`reference/pdg2png_reference.py`, and `ccitt_tables.h` were serialized or
rearranged from pdfminer.six's `ccitt.py` code trees.

- Project: https://github.com/pdfminer/pdfminer.six
- Source: https://github.com/pdfminer/pdfminer.six/blob/master/pdfminer/ccitt.py
- License: MIT
- Copyright notice: [licenses/pdfminer_CCITT_LICENSE.txt](licenses/pdfminer_CCITT_LICENSE.txt)

The PDG container parsing, payload restoration, and converter integration are
the project's implementation. pdfminer.six is not a runtime dependency.

## Pillow

The Python implementation uses Pillow, installed separately through
`requirements.txt`. Pillow is not vendored in this repository.

- Project and license: https://github.com/python-pillow/Pillow

## Windows system components

The C implementation uses Windows BCrypt and Windows Imaging Component (WIC).
These components are supplied by Windows and are not redistributed here.

The analyzed Pdg2Pic executable, its bundled libraries, and the original book
samples are not distributed in this repository.
