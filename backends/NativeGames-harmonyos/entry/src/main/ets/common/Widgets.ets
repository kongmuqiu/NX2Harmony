// The shell's game-style pieces: a chunky button with a badge that bounces when pressed, a status chip, a section
// title. Colours from Theme.ets and the game's accent (GameProfile.ets).
import { badge } from './Icons';
import { Ui, alpha } from './Theme';
import { GameProfile } from './GameProfile';

@Component
export struct GameButton {
  @Prop label: string = '';
  @Prop badgeName: string = '';
  @Prop primary: boolean = false;
  @Prop big: boolean = false;
  @Prop active: boolean = true;
  @Prop fill: boolean = false;   // as wide as its parent
  @State private pressed: boolean = false;
  onTap: () => void = () => {};

  build() {
    Row() {
      if (this.badgeName.length > 0) {
        Image(badge(this.badgeName)).width(this.big ? 34 : 26).height(this.big ? 34 : 26).margin({ right: this.big ? 12 : 8 })
      }
      Text(this.label)
        .fontSize(this.big ? 20 : 15)
        .fontWeight(FontWeight.Bolder)
        .fontColor(this.primary ? '#0B1118' : Ui.text)
        .letterSpacing(this.big ? 2 : 0.5)
    }
    .justifyContent(FlexAlign.Center)
    .width(this.fill ? '100%' : undefined)
    .height(this.big ? 62 : 44)
    .padding({ left: this.big ? 22 : 14, right: this.big ? 22 : 16 })
    .borderRadius({ topLeft: this.big ? 22 : 14, bottomRight: this.big ? 22 : 14, topRight: 6, bottomLeft: 6 })
    .linearGradient(this.primary ? { angle: 100, colors: [[GameProfile.accent, 0], [GameProfile.accent2, 1]] } :
      { angle: 180, colors: [[Ui.card2, 0], [Ui.card, 1]] })
    .border({ width: this.primary ? 0 : 1, color: Ui.line })
    .shadow(this.primary ? { radius: this.pressed ? 10 : 22, color: alpha(GameProfile.accent, 0.45), offsetY: 4 } :
      { radius: 6, color: '#66000000', offsetY: 3 })
    .opacity(this.active ? 1 : 0.45)
    .scale({ x: this.pressed ? 0.95 : 1, y: this.pressed ? 0.95 : 1 })
    .animation({ duration: 160, curve: Curve.Friction })
    .onTouch((e: TouchEvent) => {
      if (!this.active) return;
      if (e.type === TouchType.Down) this.pressed = true;
      if (e.type === TouchType.Up || e.type === TouchType.Cancel) this.pressed = false;
    })
    .onClick(() => { if (this.active) this.onTap(); })
  }
}

// A state at a glance: a badge, a name and a value coloured by state (0 good, 1 attention, 2 missing).
@Component
export struct StatusChip {
  @Prop badgeName: string = 'info';
  @Prop label: string = '';
  @Prop value: string = '';
  @Prop state: number = 0;

  build() {
    Column() {
      Row() {
        Image(badge(this.badgeName)).width(22).height(22).margin({ right: 6 })
        Text(this.label).fontSize(11).fontColor(Ui.text3).maxLines(1)
      }
      Text(this.value).fontSize(15).fontWeight(FontWeight.Bolder)
        .fontColor(this.state === 0 ? Ui.text : this.state === 1 ? Ui.warn : Ui.bad)
        .maxLines(1).margin({ top: 4 })
    }
    .alignItems(HorizontalAlign.Start)
    .padding({ left: 10, right: 8, top: 8, bottom: 8 })
    .borderRadius({ topLeft: 14, bottomRight: 14, topRight: 6, bottomLeft: 6 })
    .backgroundColor(alpha(Ui.card, 0.85))
    .border({ width: 1, color: this.state === 2 ? alpha(Ui.bad, 0.6) : Ui.line })
  }
}

// A section's title: a badge, bold text and the game's accent bar.
@Component
export struct SectionTitle {
  @Prop badgeName: string = 'info';
  @Prop title: string = '';
  @Prop note: string = '';

  build() {
    Row() {
      Image(badge(this.badgeName)).width(26).height(26).margin({ right: 8 })
      Text(this.title).fontSize(16).fontWeight(FontWeight.Bolder).fontColor(Ui.text)
      Blank()
      if (this.note.length > 0) {
        Text(this.note).fontSize(11).fontColor(Ui.text3)
      }
    }.width('100%').margin({ bottom: 8 })
  }
}
