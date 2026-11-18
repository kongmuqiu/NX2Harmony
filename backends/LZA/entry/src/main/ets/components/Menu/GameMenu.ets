// The in-game menu (the game page's menu button, or the back gesture), a pause screen: continue across the top, then
// four tiles: settings, the on-screen keys (auto: hidden while a controller is connected / always shown / hidden),
// the camera control (swipe or two sticks) and back to the start page. A dimmed backdrop closes it.
import { badge, icon } from '../../common/Icons';
import { Ui, alpha } from '../../common/Theme';
import { GameProfile } from '../../common/GameProfile';

@Component
struct MenuTile {
  @Prop badgeName: string = 'info';
  @Prop label: string = '';
  @Prop value: string = '';
  @State private pressed: boolean = false;
  onTap: () => void = () => {};

  build() {
    Row() {
      Image(badge(this.badgeName)).width(38).height(38).margin({ right: 12 })
      Column() {
        Text(this.label).fontSize(16).fontWeight(FontWeight.Bolder).fontColor(Ui.text)
        if (this.value.length > 0) {
          Text(this.value).fontSize(12).fontWeight(FontWeight.Bold).fontColor(GameProfile.accent).margin({ top: 2 })
        }
      }.alignItems(HorizontalAlign.Start).layoutWeight(1)
    }
    .layoutWeight(1)
    .height(66)
    .padding({ left: 12, right: 12 })
    .borderRadius({ topLeft: 16, bottomRight: 16, topRight: 6, bottomLeft: 6 })
    .backgroundColor(this.pressed ? alpha(GameProfile.accent, 0.22) : alpha(Ui.card2, 0.92))
    .border({ width: 1, color: this.pressed ? GameProfile.accent : Ui.line })
    .scale({ x: this.pressed ? 0.96 : 1, y: this.pressed ? 0.96 : 1 })
    .animation({ duration: 140, curve: Curve.Friction })
    .onTouch((e: TouchEvent) => {
      if (e.type === TouchType.Down) this.pressed = true;
      if (e.type === TouchType.Up || e.type === TouchType.Cancel) this.pressed = false;
    })
    .onClick(() => { this.onTap(); })
  }
}

export function touchModeText(mode: string): string {
  return mode === 'show' ? '一直显示' : mode === 'hide' ? '隐藏' : '自动';
}

@Component
export struct GameMenu {
  @Prop touchMode: string = 'auto';
  @Prop dualStick: boolean = false;
  @Prop standard: boolean = false;
  @State private shown: boolean = false;
  @State private resumePressed: boolean = false;
  onResume: () => void = () => {};
  onSettings: () => void = () => {};
  onTouchMode: () => void = () => {};
  onToggleLook: () => void = () => {};
  onLayout: () => void = () => {};
  onHome: () => void = () => {};

  aboutToAppear(): void {
    this.getUIContext().animateTo({ duration: 220, curve: Curve.Friction }, () => { this.shown = true; });
  }

  build() {
    Stack() {
      Column().width('100%').height('100%').backgroundColor('#B3000000').onClick(() => { this.onResume(); })
      Column({ space: 10 }) {
        Row() {
          Image($r('app.media.game_icon')).width(42).height(42).borderRadius(12).margin({ right: 12 })
          Column() {
            Text('游 戏 暂 停').fontSize(11).fontWeight(FontWeight.Bolder).fontColor(GameProfile.accent)
            Text(GameProfile.title).fontSize(17).fontWeight(FontWeight.Bolder).fontColor(Ui.text).maxLines(1)
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          Stack() {
            Image(icon('close')).width(16).height(16).fillColor(Ui.text2)
          }.width(36).height(36).borderRadius(18).backgroundColor(alpha(Ui.card2, 0.9))
          .onClick(() => { this.onResume(); })
        }.width('100%')
        Row() {
          Image(badge('play')).width(36).height(36).margin({ right: 12 })
          Text('继续游戏').fontSize(19).fontWeight(FontWeight.Bolder).fontColor('#0B1118').letterSpacing(2)
        }
        .width('100%')
        .height(58)
        .justifyContent(FlexAlign.Center)
        .borderRadius({ topLeft: 18, bottomRight: 18, topRight: 6, bottomLeft: 6 })
        .linearGradient({ angle: 100, colors: [[GameProfile.accent, 0], [GameProfile.accent2, 1]] })
        .shadow({ radius: this.resumePressed ? 8 : 20, color: alpha(GameProfile.accent, 0.5), offsetY: 4 })
        .scale({ x: this.resumePressed ? 0.97 : 1, y: this.resumePressed ? 0.97 : 1 })
        .animation({ duration: 140, curve: Curve.Friction })
        .onTouch((e: TouchEvent) => {
          if (e.type === TouchType.Down) this.resumePressed = true;
          if (e.type === TouchType.Up || e.type === TouchType.Cancel) this.resumePressed = false;
        })
        .onClick(() => { this.onResume(); })
        Row({ space: 10 }) {
          MenuTile({ badgeName: 'settings', label: '游戏设置', value: '应用后重启', onTap: () => { this.onSettings(); } })
          MenuTile({ badgeName: this.touchMode === 'hide' ? 'eye_off' : 'touch', label: '触屏按键',
            value: touchModeText(this.touchMode), onTap: () => { this.onTouchMode(); } })
          MenuTile({ badgeName: 'controller', label: '按键布局', value: this.standard ? '标准手柄' : '游戏定制',
            onTap: () => { this.onLayout(); } })
        }.width('100%')
        Row({ space: 10 }) {
          MenuTile({ badgeName: this.dualStick ? 'sticks' : 'look', label: '视角操作', value: this.dualStick ? '双摇杆' : '划屏',
            onTap: () => { this.onToggleLook(); } })
          MenuTile({ badgeName: 'home', label: '回到启动页', value: '会结束游戏', onTap: () => { this.onHome(); } })
        }.width('100%')
      }
      .width(660)
      .padding(16)
      .borderRadius({ topLeft: 28, bottomRight: 28, topRight: 10, bottomLeft: 10 })
      .backgroundColor(alpha(Ui.bg2, 0.95))
      .border({ width: 1.5, color: alpha(GameProfile.accent, 0.55) })
      .shadow({ radius: 44, color: alpha(GameProfile.accent, 0.28) })
      .scale({ x: this.shown ? 1 : 0.9, y: this.shown ? 1 : 0.9 })
      .opacity(this.shown ? 1 : 0)
    }
    .width('100%')
    .height('100%')
  }
}
