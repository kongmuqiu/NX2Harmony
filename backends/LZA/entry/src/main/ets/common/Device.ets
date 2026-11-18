// The device facts the pages share: the screen's sides and the game version the native runtime was built for.
import { display } from '@kit.ArkUI';
import totk from 'libtotk_ohos.so';

// The game version the native runtime was built for: the settings page shows the options for it.
export function runtimeGameVersion(): string {
  try {
    return totk.gameVersion();
  } catch (e) {
    return '1.0.0';
  }
}

export function screenLongSide(): number {
  try {
    const d = display.getDefaultDisplaySync();
    return Math.max(d.width, d.height);
  } catch (e) {
    return 1920;
  }
}

export function screenShortSide(): number {
  try {
    const d = display.getDefaultDisplaySync();
    return Math.min(d.width, d.height);
  } catch (e) {
    return 0;
  }
}
