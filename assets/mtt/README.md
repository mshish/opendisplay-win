# MTT user_edid.bin

Fixed EDID 1.3 (128 bytes) for OpenDisplay MTT CustomEdid.

## Identity (stable — do not randomize)
- Manufacturer: MTT
- Product: 0x1337
- Serial: 0x4F445731 ("ODW1")

## Modes @ 60 Hz (DTD0 = preferred)
- 2352x1632
- 2384x1664
- 1760x1216
- 1168x816

## Regenerate
```
python tools/gen_user_edid.py
```
Writes `assets/mtt/user_edid.bin`. Decode:
```
python tools/gen_user_edid.py --decode assets/mtt/user_edid.bin
```

## Install (when it is safe to restart the VDD)
1. Copy bin to `C:\VirtualDisplayDriver\user_edid.bin`
2. Set `<CustomEdid>true</CustomEdid>` and `<PreventSpoof>true</PreventSpoof>` in `vdd_settings.xml`
3. Keep XML `<resolutions>` mirrored to the same four modes (IddCx QueryTargetModes still reads XML; CustomEdid alone does not emulate modes)
4. Reload MttVDD (`pnputil /restart-device` on Root\MttVDD, or run `opendisplay-win --ensure-mtt-edid` elevated)

App path: `EnsureCustomEdid()` / `EnsureMttResolutionsForHello` installs the shipped asset and sets flags; no longer rewrites resolutions from hello sizes.
