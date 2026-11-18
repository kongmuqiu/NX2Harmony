"""Readers for the player's own Switch game files (card images, NSP / NSZ, NCA, RomFS), vendored unchanged except for
package-relative imports from totk-android games/totk/scripts at e3c915fc1bb6 (MIT, see LICENSE in this folder):
extract_app_icon.py, extract_exefs.py, extract_romfs.py, extract_shared_fonts.py. NX2Harmony uses them to read a
game's icon from its control data in-process (nx2h/icon.py), so the packed app needs no Python of the PC for it.
prod.keys is read by them as their input; nothing of it is printed or written anywhere."""
