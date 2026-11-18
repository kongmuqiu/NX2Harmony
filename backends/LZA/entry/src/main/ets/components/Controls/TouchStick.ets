import totk from 'libtotk_ohos.so';   // the native runtime (native-harmonyos/ohos/frontend_ohos.c)
import { readTouchTimestamp } from './EventClock';

@Component
export struct TouchStick {
  @Prop side: number = 0;
  @Prop diameter: number = 116;
  @Prop @Watch('resetControl') controlEnabled: boolean = true;
  @Prop @Watch('resetControl') resetVersion: number = 0;
  @State private offsetX: number = 0;
  @State private offsetY: number = 0;
  @State private pressed: boolean = false;
  private pointer: number = -1;
  private epoch: number = 0;
  private downTimestamp: number = 0;

  private resetControl(): void {
    if (this.pointer >= 0) totk.releaseTouchPointer(this.pointer, this.epoch);
    this.pointer = -1;
    this.offsetX = 0;
    this.offsetY = 0;
    this.pressed = false;
  }

  aboutToDisappear(): void { this.resetControl(); }

  private move(touch: TouchObject): void {
    const radius = this.diameter * 0.34;
    let x = (touch.x - this.diameter / 2) / radius;
    let y = (this.diameter / 2 - touch.y) / radius;
    const length = Math.sqrt(x * x + y * y);
    if (length > 1) { x /= length; y /= length; }
    if (!totk.setTouchStick(this.pointer, this.side, x, y, this.epoch)) {
      this.resetControl();
      return;
    }
    this.offsetX = x * radius;
    this.offsetY = -y * radius;
    this.pressed = true;
  }

  private touch(event: TouchEvent): void {
    const timestamp = readTouchTimestamp(event);
    if (!Number.isFinite(timestamp)) return;
    if (event.type === TouchType.Cancel) {
      if (timestamp >= this.downTimestamp) this.resetControl();
      return;
    }
    event.changedTouches.forEach((touch: TouchObject) => {
      if (event.type === TouchType.Down && this.controlEnabled && this.pointer < 0) {
        this.pointer = touch.id;
        this.epoch = totk.getTouchInputEpoch();
        this.downTimestamp = timestamp;
        this.move(touch);
      } else if (touch.id === this.pointer && timestamp >= this.downTimestamp && event.type === TouchType.Move) {
        this.move(touch);
      } else if (touch.id === this.pointer && timestamp >= this.downTimestamp && event.type === TouchType.Up) {
        this.resetControl();
      }
    });
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      Circle().width(this.diameter).height(this.diameter)
        .fill('#2637434C').stroke(this.pressed ? '#85B7C9' : '#5C83929F').strokeWidth(1)
        .hitTestBehavior(HitTestMode.None)
      Circle().width(this.diameter * 0.67).height(this.diameter * 0.67)
        .fill(Color.Transparent).stroke('#415D6B78').strokeWidth(1)
        .hitTestBehavior(HitTestMode.None)
      Text(this.side === 0 ? '移动' : '视角')
        .fontSize(11).fontColor('#8DB1BDC7')
        .translate({ y: this.diameter * 0.34 })
        .hitTestBehavior(HitTestMode.None)
      Circle().width(this.diameter * 0.39).height(this.diameter * 0.39)
        .fill(this.pressed ? '#A654A3BD' : '#806E8798')
        .stroke(this.pressed ? '#C8F2FD' : '#8599AEBB').strokeWidth(1)
        .translate({ x: this.offsetX, y: this.offsetY })
        .hitTestBehavior(HitTestMode.None)
    }
    .width(this.diameter).height(this.diameter)
    .opacity(this.controlEnabled ? 1 : 0.4)
    .hitTestBehavior(HitTestMode.Block)
    .onTouch((event: TouchEvent) => { this.touch(event); })
    .accessibilityText(this.side === 0 ? '移动摇杆' : '视角摇杆')
    .accessibilityDescription('固定中心，松开回中')
  }
}
