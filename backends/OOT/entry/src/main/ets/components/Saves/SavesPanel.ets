// The launcher's saves page: the current save slots as cards, export / import as a zip, and the local backups with
// restore. Every import and restore first backs the current saves up (SaveStore), so nothing is lost by a wrong pick.
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { SaveStore, SaveSlot, SaveBackup, formatTime } from './SaveStore';
import { badge, icon } from '../../common/Icons';
import { Ui, alpha } from '../../common/Theme';
import { GameProfile } from '../../common/GameProfile';
import { GameButton, SectionTitle } from '../../common/Widgets';
import totk from 'libtotk_ohos.so';

@Component
export struct SavesPanel {
  @State slots: SaveSlot[] = [];
  @State backups: SaveBackup[] = [];
  @State busy: boolean = false;
  @State message: string = '';
  @State failed: boolean = false;
  onChanged: () => void = () => {};

  private store(): SaveStore {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    return new SaveStore(context.filesDir, context.cacheDir);
  }

  aboutToAppear(): void { this.refresh(); }

  private refresh(): void {
    try {
      const s = this.store();
      this.slots = s.currentSlots();
      this.backups = s.backups();
    } catch (e) {
      this.say('读取存档失败：' + String(e), true);
    }
    this.onChanged();
  }

  private say(text: string, failed: boolean): void {
    this.message = text;
    this.failed = failed;
    if (failed) hilog.error(0x4D47, 'NGSaves', '%{public}s', text);
  }

  private async exportSaves(): Promise<void> {
    this.busy = true;
    try {
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const name = await this.store().exportZip(context);
      this.say(name.length > 0 ? '已导出：' + name : '已取消', false);
    } catch (e) {
      this.say('导出失败：' + ((e as Error).message ?? String(e)), true);
    }
    this.busy = false;
  }

  private async importSaves(): Promise<void> {
    if (totk.isStarted()) { this.say('游戏正在运行，重新打开应用后再导入', true); return; }
    this.busy = true;
    try {
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const n = await this.store().importZip(context);
      this.say(n < 0 ? '已取消' : '已导入 ' + n + ' 个存档位；原来的存档已备份', false);
    } catch (e) {
      this.say('导入失败：' + ((e as Error).message ?? String(e)), true);
    }
    this.busy = false;
    this.refresh();
  }

  private confirmRestore(b: SaveBackup): void {
    this.getUIContext().showAlertDialog({
      title: '恢复这份备份？',
      message: formatTime(b.time) + '（' + b.slots + ' 个存档位）。当前的存档会先备份。',
      primaryButton: { value: '取消', action: () => {} },
      secondaryButton: { value: '恢复', action: () => { this.restore(b); } }
    });
  }

  private restore(b: SaveBackup): void {
    if (totk.isStarted()) { this.say('游戏正在运行，重新打开应用后再恢复', true); return; }
    try {
      this.store().restore(b);
      this.say('已恢复 ' + formatTime(b.time) + ' 的备份', false);
    } catch (e) {
      this.say('恢复失败：' + ((e as Error).message ?? String(e)), true);
    }
    this.refresh();
  }

  @Builder
  private slotCard(s: SaveSlot) {
    Row() {
      Image(badge('slot')).width(30).height(30).margin({ right: 10 })
      Column() {
        Text('存档位 ' + s.index).fontSize(14).fontWeight(FontWeight.Bolder).fontColor(Ui.text)
        Text(formatTime(s.time)).fontSize(11).fontColor(Ui.text3).margin({ top: 2 })
      }.alignItems(HorizontalAlign.Start)
    }
    .padding({ left: 10, right: 14, top: 8, bottom: 8 })
    .margin({ right: 8, bottom: 8 })
    .borderRadius({ topLeft: 14, bottomRight: 14, topRight: 6, bottomLeft: 6 })
    .backgroundColor(Ui.card2)
    .border({ width: 1, color: Ui.line })
  }

  @Builder
  private backupRow(b: SaveBackup) {
    Row() {
      Image(badge('clock')).width(26).height(26).margin({ right: 10 })
      Column() {
        Text(formatTime(b.time)).fontSize(13).fontWeight(FontWeight.Bold).fontColor(Ui.text)
        Text(b.slots + ' 个存档位').fontSize(11).fontColor(Ui.text3)
      }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      Row() {
        Image(icon('restore')).width(16).height(16).fillColor('#0B1118').margin({ right: 4 })
        Text('恢复').fontSize(13).fontWeight(FontWeight.Bolder).fontColor('#0B1118')
      }
      .padding({ left: 10, right: 12, top: 6, bottom: 6 })
      .borderRadius({ topLeft: 10, bottomRight: 10, topRight: 4, bottomLeft: 4 })
      .backgroundColor(GameProfile.accent)
      .opacity(this.busy ? 0.4 : 1)
      .onClick(() => { if (!this.busy) this.confirmRestore(b); })
    }
    .width('100%')
    .padding({ top: 8, bottom: 8 })
    .border({ width: { bottom: 1 }, color: Ui.line })
  }

  build() {
    Column() {
      SectionTitle({ badgeName: 'save', title: '当前存档', note: this.slots.length > 0 ? this.slots.length + ' 个存档位' : '' })
      if (this.slots.length === 0) {
        Text('还没有存档：开始游戏并存一次档后会出现在这里').fontSize(12).fontColor(Ui.text3).margin({ bottom: 8 })
      } else {
        Flex({ wrap: FlexWrap.Wrap }) {
          ForEach(this.slots, (s: SaveSlot) => { this.slotCard(s) }, (s: SaveSlot) => String(s.index) + ':' + s.time)
        }
      }
      Row({ space: 10 }) {
        GameButton({ label: '导出存档', badgeName: 'export', active: !this.busy, onTap: () => { this.exportSaves(); } })
        GameButton({ label: '导入存档', badgeName: 'import', active: !this.busy, onTap: () => { this.importSaves(); } })
        if (this.busy) { LoadingProgress().width(26).height(26).color(GameProfile.accent) }
      }.margin({ top: 4, bottom: 6 })
      Text('导出为 zip，存到你选的位置；导入可以用本应用导出的 zip，也可以用存档管理工具导出的整套存档（含 ' +
        GameProfile.saveExample + '）。')
        .fontSize(11).fontColor(Ui.text3)
      if (this.message.length > 0) {
        Text(this.message).fontSize(12).fontWeight(FontWeight.Bold).fontColor(this.failed ? Ui.bad : Ui.ok).margin({ top: 6 })
      }
      SectionTitle({ badgeName: 'restore', title: '自动备份', note: '保留最近 10 份' }).margin({ top: 14 })
      Text('每次开始游戏前、导入和恢复前都会自动备份。').fontSize(11).fontColor(Ui.text3).margin({ bottom: 4 })
      Scroll() {
        Column() {
          if (this.backups.length === 0) {
            Text('还没有备份').fontSize(13).fontColor(Ui.text3)
          }
          ForEach(this.backups, (b: SaveBackup) => { this.backupRow(b) }, (b: SaveBackup) => b.name)
        }.width('100%').alignItems(HorizontalAlign.Start)
      }.layoutWeight(1).scrollBar(BarState.Off).align(Alignment.Top).edgeEffect(EdgeEffect.Spring)
    }
    .width('100%').height('100%')
    .alignItems(HorizontalAlign.Start)
  }
}
