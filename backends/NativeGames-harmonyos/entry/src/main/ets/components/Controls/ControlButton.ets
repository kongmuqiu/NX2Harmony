import totk from 'libtotk_ohos.so';   // the native runtime (native-harmonyos/ohos/frontend_ohos.c)
import { readTouchTimestamp } from './EventClock';
import { icon } from '../../common/Icons';
import { alpha } from '../../common/Theme';

interface HeldTouch {
  id: number;
  epoch: number;
  downTimestamp: number;
}

// Each finger keeps its Down epoch and event timestamp. Native epochs reject
// former-session tokens; timestamp ordering also rejects an older queued event
// when the OS has reused its pointer ID for a newly captured gesture.
@Component
export struct ControlButton {
  @Prop label: string = '';
  @Prop glyph: string = '';     // common/Icons.ets icon() name; '' shows the label
  @Prop tint: string = '';      // the ring's colour by kind of action; '' neutral
  @Prop @Watch('resetControl') padMask: number = 0;
  // A verified game-state provider can change an action's identity while its
  // HID mask stays equal. Cancel only this control's old pointers in that case.
  @Prop @Watch('resetControl') bindingKey: string = '';
  @Prop buttonSize: number = 48;
  @Prop buttonWidth: number = 0;
  @Prop @Watch('resetControl') controlEnabled: boolean = true;
  @Prop @Watch('resetControl') resetVersion: number = 0;
  @State private pressed: boolean = false;
  private held: HeldTouch[] = [];

  private resetControl(): void {
    this.held.forEach((touch: HeldTouch) => {
      totk.cancelTouchPointer(touch.id, touch.epoch);
    });
    this.held = [];
    this.pressed = false;
  }

  aboutToDisappear(): void { this.resetControl(); }

  private touch(event: TouchEvent): void {
    const timestamp = readTouchTimestamp(event);
    if (!Number.isFinite(timestamp)) return;
    if (event.type === TouchType.Cancel) {
      this.held = this.held.filter((touch: HeldTouch) => {
        if (timestamp < touch.downTimestamp) return true;
        totk.cancelTouchPointer(touch.id, touch.epoch);
        return false;
      });
      this.pressed = this.held.length > 0;
      return;
    }
    event.changedTouches.forEach((touch: TouchObject) => {
      if (event.type === TouchType.Down && this.controlEnabled && this.padMask !== 0) {
        if (this.held.some((item: HeldTouch) => item.id === touch.id)) return;
        const epoch = totk.getTouchInputEpoch();
        if (totk.setTouchButton(touch.id, this.padMask, true, epoch)) {
          this.held.push({ id: touch.id, epoch: epoch, downTimestamp: timestamp });
        }
      } else if (event.type === TouchType.Up) {
        const item = this.held.find((held: HeldTouch) => held.id === touch.id);
        if (!item || timestamp < item.downTimestamp) return;
        totk.releaseTouchPointer(item.id, item.epoch);
        this.held = this.held.filter((held: HeldTouch) => held.id !== touch.id);
      }
    });
    this.pressed = this.held.length > 0;
  }

  private ring(): string { return this.tint.length > 0 ? this.tint : '#9DB0C4'; }

  @Builder
  private face() {
    if (this.glyph.length === 0) {
      Text(this.label)
        .fontSize(this.label.length > 2 ? 12 : this.buttonSize * 0.34)
        .fontWeight(FontWeight.Bolder)
        .fontColor(this.controlEnabled ? '#F4F8FC' : '#68747D')
        .textAlign(TextAlign.Center)
        .maxLines(1)
        .hitTestBehavior(HitTestMode.None)
    } else if (this.buttonWidth > 0) {
      Row() {
        Image(icon(this.glyph)).width(16).height(16).fillColor('#F4F8FC').margin({ right: 4 })
        Text(this.label).fontSize(11).fontWeight(FontWeight.Bold).fontColor('#F4F8FC').maxLines(1)
      }.hitTestBehavior(HitTestMode.None)
    } else {
      Column() {
        Image(icon(this.glyph)).width(this.buttonSize * (this.buttonSize >= 46 ? 0.42 : 0.5))
          .height(this.buttonSize * (this.buttonSize >= 46 ? 0.42 : 0.5))
          .fillColor(this.controlEnabled ? '#F4F8FC' : '#68747D')
        if (this.buttonSize >= 46) {
          Text(this.label).fontSize(9).fontWeight(FontWeight.Bold).fontColor(this.controlEnabled ? '#DCE6F0' : '#68747D')
            .margin({ top: 1 }).maxLines(1)
        }
      }.hitTestBehavior(HitTestMode.None)
    }
  }

  build() {
    Button({ type: this.buttonWidth > 0 ? ButtonType.Capsule : ButtonType.Circle }) {
      this.face()
    }
    .width(this.buttonWidth > 0 ? this.buttonWidth : this.buttonSize)
    .height(this.buttonSize)
    .padding(0)
    .backgroundColor(this.pressed ? alpha(this.ring(), 0.55) : '#5C0E141C')
    .border({ width: this.pressed ? 2 : 1.5, color: alpha(this.ring(), this.pressed ? 1 : 0.7) })
    .shadow(this.pressed ? { radius: 16, color: alpha(this.ring(), 0.8) } : { radius: 4, color: '#40000000' })
    .stateEffect(false)
    .enabled(this.controlEnabled)
    .opacity(this.controlEnabled ? 1 : 0.45)
    .scale({ x: this.pressed ? 1.14 : 1, y: this.pressed ? 1.14 : 1, centerX: '50%', centerY: '50%' })
    .animation({ duration: 90, curve: Curve.EaseOut })
    .onTouch((event: TouchEvent) => { this.touch(event); })
    .accessibilityText(this.label)
    .accessibilityDescription(this.padMask === 0 ? '当前未接入' : '按住生效，松开释放')
  }
}
