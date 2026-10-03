# Controls

## Controller

**Nothing needs to be mapped.** The default backend is SDL, which ships native
mappings for the usual controllers. And input sources are **merged**: `MergeInto`
ORs the keyboard and controller buttons, so both work at once and enabling
`--mnk_mode` doesn't disable the controller.

### DualShock 4 / DualSense (PlayStation)

It works over USB and Bluetooth. This SDL is built with the `hidapi` driver, which is
the one that handles PlayStation controllers.

| 360 button | PlayStation button |
|---|---|
| **Start** | **Options** |
| Back | Share / Create |
| A | **Cross** |
| B | Circle |
| X | Square |
| Y | Triangle |
| LB / RB | L1 / R1 |
| LT / RT | L2 / R2 |
| Left / right stick | Left / right stick |
| Stick clicks | L3 / R3 |
| D-pad | D-pad |
| Guide | PS (has to be enabled with `--guide_button`) |

To drive: accelerate **R2**, brake **L2**, steer with the left stick.

### If the controller doesn't respond

`FASE3_RUN.bat` prints a **CONTROLLERS DETECTED** section at the end. If SDL has seen
it, an `SDL OnControllerDeviceAdded` line appears with its name and GUID. If nothing
appears:

1. **Steam running**: Steam Input hijacks PlayStation controllers and can hide them or
   present them as an Xbox controller. Close Steam or disable PlayStation compatibility
   in its controller settings.
2. **DS4Windows / DSX**: same problem, they act as a middleman. Close them.
3. **Bluetooth asleep**: press the PS button to wake it before starting the game. SDL
   enumerates at startup and also hot-plugs, but it's more reliable to have it awake
   beforehand.
4. **Manual mapping**: if SDL sees it as a joystick but not as a gamepad, you need a
   `gamecontrollerdb.txt` next to the executable. The log warns that it doesn't exist
   (`SDL GameControllerDB: file does not exist`), but it's just a warning: SDL3's
   internal mappings cover the DS4. You only need the file for unusual controllers. It's
   downloaded from the SDL_GameControllerDB project and pointed to with
   `--hid_mappings_file`.

Alternative for Xbox controllers: `--input_backend xinput`.

## Keyboard

**It has to be enabled explicitly with `--mnk_mode`.** It's off by default, which is
why no key does anything even though the bindings already exist. The project's launchers
already pass it.

| 360 button | Key |
|---|---|
| **Start** | **X** or **Enter** |
| Back | Z or Tab |
| A | `;` or Space |
| B | `'` or Backspace |
| X | L |
| Y | P |
| Left trigger (LT) | Q or I |
| Right trigger (RT) | E or O |
| Left bumper (LB) | 1 |
| Right bumper (RB) | 3 |
| Left stick | W A S D |
| Left stick click | F |
| Right stick | Arrow keys |
| Right stick click | K |
| D-pad | Shift + arrows |
| Guide | unassigned |

Watch out for the trap: the **X key is Start**, not the X button. The X button is
**L**.

### To drive

- Accelerate: **E** or **O** (right trigger)
- Brake: **Q** or **I** (left trigger)
- Steer: **A** / **D**
- Handbrake: `;` or Space (A button)

### Changing the bindings

Every button is a CVar and accepts several keys separated by commas:

```
--keybind_start "Return,Space"
--keybind_right_trigger "Up"
```

`--mnk_mouse` uses the mouse for the right stick (the camera).
`--mnk_sensitivity` adjusts its sensitivity.
