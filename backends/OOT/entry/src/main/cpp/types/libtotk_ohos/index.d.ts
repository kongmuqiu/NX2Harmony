// libtotk_ohos.so: the Android port's native stack for HarmonyOS (native-harmonyos/ohos/totk_napi.c).
// start(filesDir) runs the runtime on its own thread; returns 'started' or why it did not start.
export const start: (filesDir: string) => string;
// Whether start() has run in this process (the launcher only changes saves while the game is not running).
export const isStarted: () => boolean;
// The game version this library runs: '1.4.3' or '1.0.0'.
export const gameVersion: () => string;
// The XComponent surface for the Vulkan presenter (window build): 0 when attached.
export const setSurface: (surfaceId: string, width: number, height: number) => number;
export const surfaceChanged: (width: number, height: number) => void;
export const surfaceDestroyed: () => void;
// On-screen controller: Switch button bits and sticks (-32767..32767, up positive).
export const setPad: (buttons: number, lx: number, ly: number, rx: number, ry: number) => void;
// On-screen controls (components/Controls): per-finger buttons and sticks.
export const getTouchInputEpoch: () => number;
export const setTouchButton: (pointerId: number, mask: number, down: boolean, epoch: number) => boolean;
export const setTouchStick: (pointerId: number, side: number, x: number, y: number, epoch: number) => boolean;
// Swipe camera: the finger moved by (dx, dy) fractions of the swipe area's height (right / up positive); the runtime
// turns the distance into right-stick input as the game reads it, so the camera follows the finger and stops with it.
export const addTouchLook: (pointerId: number, dx: number, dy: number, epoch: number) => boolean;
export const releaseTouchPointer: (pointerId: number, epoch: number) => boolean;
export const cancelTouchPointer: (pointerId: number, epoch: number) => boolean;
// Physical controllers (GameControllerKit, read in the runtime): a key the page receives itself (Select / View as
// Minus, Switch bit 11), and the connected controllers (count -1 when the system has no GameControllerKit).
export const setPadKey: (bit: number, down: boolean) => void;
export interface GamepadStatus {
  count: number;
  name: string;
}
export const gamepadStatus: () => GamepadStatus;
// The loading screen: the runtime's boot stage (as the runtime names it, English) with its progress, done / total in
// bytes or, with items, in counted items; frames > 0 once the game has shown a picture; elapsed seconds since the boot.
export interface BootStatus {
  stage: string;
  detail: string;
  done: number;
  total: number;
  items: boolean;
  frames: number;
  elapsed: number;
}
export const bootStatus: () => BootStatus;
// The game page left the foreground (false: rumble stops, the motion sensors pause) or came back (true).
export const setForeground: (on: boolean) => void;
// The game's keyboard: the page's dialog gets each request and answers it once with textInputDone(serial, text), or
// textInputDone(serial, null) to cancel. The game waits until then.
export interface TextRequest {
  serial: number;
  prompt: string;
  initial: string;
  maxLength: number;
  minLength: number;
  digitsOnly: boolean;
  secure: boolean;
}
export const onTextInput: (callback: (request: TextRequest) => void) => boolean;
export const textInputDone: (serial: number, text: string | null) => boolean;
