// The game's saves on the phone: export and import as a zip through the system file picker, local backups (kept
// automatically before every start, before an import and before a restore), and restoring one. Only used from the
// launcher, while the game runtime is not running.
// The live save root is files/android/user-data/save/<uid> (runtime/fs_host.c): <uid> is account.uid, two
// little-endian 64-bit halves {lo, hi}, written as hex of hi then lo. A missing account.uid is created as the runtime
// would (16 random non-zero bytes), so saves imported before the first start land where the game looks.
// An imported zip may come from this app, from another copy of the game's port, or from a save manager: its save root
// is the shallowest folder holding a slot's progress file or a marker file (GameProfile: TOTK slot_NN/progress.sav or
// option.sav, Z-A main or backup; the newest one when several sit at that
// depth). Archives with links, more than 500 files or 128 MB under the root, or an oversized file are refused.
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { common } from '@kit.AbilityKit';
import { GameProfile } from '../../common/GameProfile';

const KEEP_BACKUPS: number = 10;
const MAX_FILES: number = 500;
const MAX_TOTAL: number = 128 * 1024 * 1024;
const MAX_KNOWN_FILE: number = 32 * 1024 * 1024;
const MAX_OTHER_FILE: number = 1024 * 1024;

export class SaveSlot {
  index: number;
  time: number;   // seconds since 1970, the slot's progress file's modification time
  constructor(index: number, time: number) { this.index = index; this.time = time; }
}

export class SaveBackup {
  name: string;
  path: string;
  time: number;
  slots: number;
  constructor(name: string, path: string, time: number, slots: number) {
    this.name = name; this.path = path; this.time = time; this.slots = slots;
  }
}

function two(n: number): string { return n < 10 ? '0' + n : String(n); }

export function formatTime(seconds: number): string {
  const d = new Date(seconds * 1000);
  return d.getFullYear() + '-' + two(d.getMonth() + 1) + '-' + two(d.getDate()) + ' ' + two(d.getHours()) + ':' + two(d.getMinutes());
}

function stamp(d: Date): string {
  return '' + d.getFullYear() + two(d.getMonth() + 1) + two(d.getDate()) + '-' + two(d.getHours()) + two(d.getMinutes()) +
    two(d.getSeconds());
}

function hexByte(b: number): string { return (b < 16 ? '0' : '') + b.toString(16); }

export class SaveStore {
  private filesDir: string;
  private cacheDir: string;

  constructor(filesDir: string, cacheDir: string) { this.filesDir = filesDir; this.cacheDir = cacheDir; }

  private profile(): string { return this.filesDir + '/android/user-data'; }
  backupsDir(): string { return this.filesDir + '/android/save-backups'; }

  // The live save root; create: make the account identity when it is missing (an import before the first start).
  root(create: boolean): string | undefined {
    const uidPath = this.profile() + '/account.uid';
    let id: Uint8Array | undefined = undefined;
    if (fs.accessSync(uidPath) && fs.statSync(uidPath).size === 16) {
      const f = fs.openSync(uidPath, fs.OpenMode.READ_ONLY);
      const buf = new ArrayBuffer(16);
      fs.readSync(f.fd, buf);
      fs.closeSync(f);
      id = new Uint8Array(buf);
    } else if (create) {
      const random = cryptoFramework.createRandom();
      do { id = random.generateRandomSync(16).data; } while (id.every((b: number) => b === 0));
      SaveStore.ensureDir(this.profile());
      const f = fs.openSync(uidPath, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(f.fd, id.buffer.slice(id.byteOffset, id.byteOffset + 16));
      fs.closeSync(f);
    }
    if (id === undefined || id.every((b: number) => b === 0)) return undefined;
    let name = '';
    for (let b = 15; b >= 8; b--) name += hexByte(id[b]);   // hi
    for (let b = 7; b >= 0; b--) name += hexByte(id[b]);    // lo
    return this.profile() + '/save/' + name;
  }

  static files(dir: string): string[] {
    if (!fs.accessSync(dir)) return [];
    return fs.listFileSync(dir, { recursion: true }).map((p: string) => p.startsWith('/') ? p.substring(1) : p);
  }

  // The slots in a save root: every file whose path matches GameProfile.saveSlot.
  static slots(dir: string | undefined): SaveSlot[] {
    const out: SaveSlot[] = [];
    if (dir === undefined || !fs.accessSync(dir)) return out;
    const slot = new RegExp('^' + GameProfile.saveSlot + '$');
    for (const rel of SaveStore.files(dir)) {
      const m = slot.exec(rel);
      if (m === null) continue;
      out.push(new SaveSlot(m.length > 1 && m[1] !== undefined ? Number(m[1]) : 0, fs.statSync(dir + '/' + rel).mtime));
    }
    return out.sort((a: SaveSlot, b: SaveSlot) => a.index - b.index);
  }


  currentSlots(): SaveSlot[] { return SaveStore.slots(this.root(false)); }

  private static copyTree(src: string, dst: string): number {
    const files = SaveStore.files(src);
    SaveStore.ensureDir(dst);
    for (const rel of files) {
      const to = dst + '/' + rel;
      const cut = to.lastIndexOf('/');
      SaveStore.ensureDir(to.substring(0, cut));
      fs.copyFileSync(src + '/' + rel, to);
    }
    return files.length;
  }

  // fs.mkdirSync(path, true) throws 'File exists' when the last folder is already there (it is not mkdir -p), so a
  // second file in one folder stopped every backup, import, restore and export; make folders only when missing.
  private static ensureDir(dir: string): void { if (!fs.accessSync(dir)) fs.mkdirSync(dir, true); }

  private static remove(dir: string): void { if (fs.accessSync(dir)) fs.rmdirSync(dir); }

  backups(): SaveBackup[] {
    const dir = this.backupsDir();
    if (!fs.accessSync(dir)) return [];
    const out: SaveBackup[] = [];
    for (const name of fs.listFileSync(dir)) {
      const path = dir + '/' + name;
      if (!fs.statSync(path).isDirectory()) continue;
      out.push(new SaveBackup(name, path, fs.statSync(path).mtime, SaveStore.slots(path).length));
    }
    return out.sort((a: SaveBackup, b: SaveBackup) => b.name.localeCompare(a.name));
  }

  // Copy the current saves to save-backups/<time>-<reason>; returns its name, or '' when there is nothing to keep.
  backup(reason: string): string {
    const root = this.root(false);
    if (root === undefined || SaveStore.files(root).length === 0) return '';
    const name = stamp(new Date()) + '-' + reason;
    SaveStore.copyTree(root, this.backupsDir() + '/' + name);
    const all = this.backups();
    for (let i = KEEP_BACKUPS; i < all.length; i++) SaveStore.remove(all[i].path);
    return name;
  }

  // Before every start: a backup when the saves changed since the last automatic one.
  autoBackup(): string {
    const root = this.root(false);
    if (root === undefined) return '';
    let signature = '';
    for (const rel of SaveStore.files(root).sort()) {
      const st = fs.statSync(root + '/' + rel);
      signature += rel + ':' + st.size + ':' + st.mtime + ';';
    }
    if (signature.length === 0) return '';
    const marker = this.backupsDir() + '/last-auto.txt';
    if (fs.accessSync(marker) && fs.readTextSync(marker) === signature) return '';
    const name = this.backup('auto');
    if (name.length > 0) {
      const f = fs.openSync(marker, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(f.fd, signature);
      fs.closeSync(f);
    }
    return name;
  }

  private replaceRootWith(source: string, files: string[]): void {
    const root = this.root(true);
    if (root === undefined) throw new Error('无法建立存档账号');
    SaveStore.remove(root);
    SaveStore.ensureDir(root);
    for (const rel of files) {
      const to = root + '/' + rel;
      SaveStore.ensureDir(to.substring(0, to.lastIndexOf('/')));
      fs.copyFileSync(source + '/' + rel, to);
    }
  }

  restore(b: SaveBackup): void {
    this.backup('before-restore');
    this.replaceRootWith(b.path, SaveStore.files(b.path));
  }

  private static copyStream(fromFd: number, toFd: number): void {
    const buf = new ArrayBuffer(1024 * 1024);
    for (;;) {
      const n = fs.readSync(fromFd, buf);
      if (n <= 0) break;
      fs.writeSync(toFd, buf, { length: n });
    }
  }

  // Zip the current saves and let the player choose where to keep the file. Returns the file name, or '' if cancelled.
  async exportZip(context: common.UIAbilityContext): Promise<string> {
    const root = this.root(false);
    if (root === undefined || SaveStore.files(root).length === 0) throw new Error('还没有存档');
    const work = this.cacheDir + '/save-export';
    SaveStore.remove(work);
    const stage = work + '/' + GameProfile.saveZip;
    SaveStore.copyTree(root, stage);
    const name = GameProfile.saveZip + ' ' + stamp(new Date()) + '.zip';
    const zip = work + '/' + name;
    await zlib.compressFile(stage, zip, { level: zlib.CompressLevel.COMPRESS_LEVEL_DEFAULT_COMPRESSION });
    const options = new picker.DocumentSaveOptions();
    options.newFileNames = [name];
    const uris = await new picker.DocumentViewPicker(context).save(options);
    if (uris.length === 0) { SaveStore.remove(work); return ''; }
    const from = fs.openSync(zip, fs.OpenMode.READ_ONLY);
    const to = fs.openSync(uris[0], fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    try { SaveStore.copyStream(from.fd, to.fd); } finally { fs.closeSync(from); fs.closeSync(to); SaveStore.remove(work); }
    return name;
  }

  private static junk(path: string): boolean {
    const name = path.substring(path.lastIndexOf('/') + 1);
    return path.startsWith('__MACOSX/') || path.includes('/__MACOSX/') || name === '.DS_Store' || name === 'Thumbs.db' ||
      name === 'desktop.ini' || name.startsWith('._');
  }

  // The save root inside an unpacked archive: '' (its top) or 'dir/sub/'.
  static pickRoot(base: string, files: string[]): string {
    const roots: Map<string, number> = new Map<string, number>();
    const slot = new RegExp('^(?:(.*)\\/)?' + GameProfile.saveSlot + '$');
    for (const p of files) {
      let root: string | undefined = undefined;
      const name = p.substring(p.lastIndexOf('/') + 1);
      if (GameProfile.saveMarkers.indexOf(name) >= 0) root = p.substring(0, p.length - name.length);
      const m = slot.exec(p);
      if (m !== null) {
        const dir: string = m[1] ?? '';
        root = dir.length > 0 ? dir + '/' : '';
      }
      if (root === undefined) continue;
      const t = m !== null ? fs.statSync(base + '/' + p).mtime : -1;
      roots.set(root, Math.max(roots.get(root) ?? -1, t));
    }
    const names: string[] = Array.from(roots.keys());
    if (names.length === 0) throw new Error('压缩包里没有找到存档（需要 ' + GameProfile.saveExample + ' 这样的文件）');
    let best: string = names[0];
    for (const r of names) {
      const depth = r.split('/').length, bestDepth = best.split('/').length;
      if (depth < bestDepth || (depth === bestDepth && (roots.get(r) ?? -1) > (roots.get(best) ?? -1))) best = r;
    }
    return best;
  }

  // Let the player pick a zip, check it, back the current saves up and put the zip's saves in their place.
  // Returns the number of save slots imported, or -1 if the picker was cancelled.
  async importZip(context: common.UIAbilityContext): Promise<number> {
    const options = new picker.DocumentSelectOptions();
    options.maxSelectNumber = 1;
    options.fileSuffixFilters = ['.zip'];
    const uris = await new picker.DocumentViewPicker(context).select(options);
    if (uris.length === 0) return -1;
    const work = this.cacheDir + '/save-import';
    SaveStore.remove(work);
    SaveStore.ensureDir(work);
    const zip = work + '/in.zip';
    const from = fs.openSync(uris[0], fs.OpenMode.READ_ONLY);
    const to = fs.openSync(zip, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    try { SaveStore.copyStream(from.fd, to.fd); } finally { fs.closeSync(from); fs.closeSync(to); }
    const unpacked = work + '/unpacked';
    SaveStore.ensureDir(unpacked);
    try {
      await zlib.decompressFile(zip, unpacked);
      const all = SaveStore.files(unpacked).filter((p: string) => !SaveStore.junk(p));
      for (const p of all) {
        if (fs.lstatSync(unpacked + '/' + p).isSymbolicLink()) throw new Error('压缩包里有链接文件：' + p);
      }
      const root = SaveStore.pickRoot(unpacked, all);
      const chosen: string[] = [];
      let total = 0;
      for (const p of all) {
        if (!p.startsWith(root)) continue;
        const rel = p.substring(root.length);
        const size = fs.statSync(unpacked + '/' + p).size;
        const known = /\.(sav|dat|jpg|jpeg|bin)$/i.test(rel) || new RegExp('^' + GameProfile.saveSlot + '$').test(rel) ||
          GameProfile.saveMarkers.indexOf(rel.substring(rel.lastIndexOf('/') + 1)) >= 0;
        const limit = known ? MAX_KNOWN_FILE : MAX_OTHER_FILE;
        if (size > limit) throw new Error('文件太大，不像存档：' + rel);
        total += size;
        chosen.push(rel);
        if (chosen.length > MAX_FILES) throw new Error('文件太多，不像存档');
        if (total > MAX_TOTAL) throw new Error('总大小超过 128 MB，不像存档');
      }
      const source = root.length > 0 ? unpacked + '/' + root.substring(0, root.length - 1) : unpacked;
      const slots = SaveStore.slots(source).length;
      this.backup('before-import');
      this.replaceRootWith(source, chosen);
      return slots;
    } finally {
      SaveStore.remove(work);
    }
  }
}
