import { ControlButton } from './ControlButton';
import { TouchStick } from './TouchStick';
import { SwipeLook } from './SwipeLook';
import { KeySpec } from './KeySpec';
import { ACTION_KEYS } from '../../common/GameProfile';
import { STANDARD_KEYS } from './StandardKeys';

// The on-screen controller: the game's keys (common/GameProfile.ets ACTION_KEYS: standard nn::hid button bits, so
// independent of the game version), the left stick, and the camera as a swipe area or a right stick. Actions with
// no verified game-state hook stay dimmed. The menu button and the camera switch are in the game menu.
@Component
export struct PhoneControls {
  @Prop areaWidth: number = 0;
  @Prop areaHeight: number = 0;
  @Prop controlEnabled: boolean = true;
  @Prop resetVersion: number = 0;
  @Prop dualStick: boolean = false;
  @Prop standard: boolean = false;   // the Switch's own layout (StandardKeys.ets) instead of the game's

  private keySize(): number { return Math.max(42, Math.min(54, this.areaHeight * 0.137)); }
  private stickSize(): number { return Math.max(100, Math.min(128, this.areaHeight * 0.33)); }
  private x(fraction: number, width: number): number {
    return Math.max(width * 0.08 + 4, Math.min(this.areaWidth - width * 1.08 - 4,
      this.areaWidth * fraction - width / 2));
  }
  private y(fraction: number, height: number): number {
    return Math.max(height * 0.08 + 4, Math.min(this.areaHeight - height * 1.08 - 4,
      this.areaHeight * fraction - height / 2));
  }

  @Builder
  private keyButton(k: KeySpec) {
    if (k.pill > 0) {
      ControlButton({ label: k.label, glyph: k.icon, tint: k.tint, padMask: k.mask, buttonSize: this.keySize() * k.factor,
        buttonWidth: this.keySize() * k.pill, controlEnabled: this.controlEnabled && k.available, resetVersion: this.resetVersion })
        .position({ x: this.x(k.x, this.keySize() * k.pill), y: this.y(k.y, this.keySize() * k.factor) })
    } else {
      ControlButton({ label: k.label, glyph: k.icon, tint: k.tint, padMask: k.mask, buttonSize: this.keySize() * k.factor,
        controlEnabled: this.controlEnabled && k.available, resetVersion: this.resetVersion })
        .position({ x: this.x(k.x, this.keySize() * k.factor), y: this.y(k.y, this.keySize() * k.factor) })
    }
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      if (!this.dualStick) {
        SwipeLook({ areaWidth: this.areaWidth * 0.65, areaHeight: this.areaHeight,
          controlEnabled: this.controlEnabled, resetVersion: this.resetVersion })
          .position({ x: this.areaWidth * 0.35, y: 0 })
      }
      TouchStick({ side: 0, diameter: this.stickSize(), controlEnabled: this.controlEnabled, resetVersion: this.resetVersion })
        .position({ x: this.x(0.15, this.stickSize()), y: this.y(0.79, this.stickSize()) })
      if (this.dualStick) {
        TouchStick({ side: 1, diameter: this.stickSize(), controlEnabled: this.controlEnabled, resetVersion: this.resetVersion })
          .position({ x: this.x(0.58, this.stickSize()), y: this.y(0.79, this.stickSize()) })
      }
      ForEach(this.standard ? STANDARD_KEYS : ACTION_KEYS, (k: KeySpec) => { this.keyButton(k) },
        (k: KeySpec) => (this.standard ? 's' : 'g') + k.label + k.mask)
    }
    .width(this.areaWidth).height(this.areaHeight)
    .hitTestBehavior(HitTestMode.Transparent)
  }
}
