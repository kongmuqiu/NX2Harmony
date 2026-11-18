// Application-private diagnostics. No native signal handlers or core changes.
// API26 declarations: PerformanceAnalysisKit hiAppEvent and FaultLogger.query.
import { hiAppEvent, FaultLogger, hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo } from '@kit.CoreFileKit';
import { util } from '@kit.ArkTS';
import { bundleManager } from '@kit.AbilityKit';

const FILE_NAME: string = 'native-faults.json';
// This app's bundle name as installed (a build may name the app differently from AppScope's default).
function selfBundle(): string {
  try {
    return bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT).name;
  } catch (e) {
    return 'TotZ.NativeGames.harmonyos';
  }
}
const SELF_BUNDLE: string = selfBundle();
const MAX_RECORDS: number = 10;
const MAX_LOG_BYTES: number = 8 * 1024 * 1024;
const MAX_ARCHIVE_BYTES: number = 24 * 1024 * 1024;
const MAX_EVENT_PARAMS_CHARS: number = 256 * 1024;

interface CrashEventParams {
  bundle_name?: string;
  time?: number;
  pid?: number;
  uid?: number;
  uuid?: string;
  crash_type?: string;
  external_log?: string[];
  log_over_limit?: boolean;
}
interface ExternalCrashLog {
  name: string;
  fileBytes: number;
  fullLog: string;
  complete: boolean;
  error: string;
}
interface NativeFaultRecord {
  key: string;
  source: string;
  capturedAt: string;
  timestamp: number;
  pid: number;
  uid: number;
  crashType: string;
  reason: string;
  module: string;
  summary: string;
  fullLog: string;
  fullLogBytes: number;
  fullLogComplete: boolean;
  paramsJson: string;
  paramsComplete: boolean;
  logOverLimit: boolean;
  externalLogs: ExternalCrashLog[];
  error: string;
}
interface NativeFaultArchive {
  schemaVersion: number;
  selfBundle: string;
  capturedAt: string;
  recordLimit: number;
  maximumLogBytes: number;
  maximumArchiveBytes: number;
  evictedRecords: number;
  watcherRegistered: boolean;
  historicalQueryDeprecatedSince: number;
  historicalQueries: number;
  watcherError: string;
  queryError: string;
  archiveReadError: string;
  records: NativeFaultRecord[];
}

const encoder: util.TextEncoder = new util.TextEncoder();
let filesDirectory: string = '';
let loaded: boolean = false;
let watcherRegistered: boolean = false;
let watcherHolder: hiAppEvent.AppEventPackageHolder | undefined = undefined;
let historyQueried: boolean = false;
let historicalQueries: number = 0;
let evictedRecords: number = 0;
let watcherError: string = '';
let queryError: string = '';
let archiveReadError: string = '';
let saveError: string = '';
let savedAt: string = '';
let savedBytes: number = 0;
let records: NativeFaultRecord[] = [];
let queueTail: Promise<void> = Promise.resolve();

function ErrorText(error: Object): string {
  let text: string = String(error);
  try { text += ' ' + JSON.stringify(error); } catch (_) { }
  return text.slice(0, 2048);
}

function Retain(record: NativeFaultRecord): void {
  const index = records.findIndex((item: NativeFaultRecord) => item.key === record.key);
  if (index >= 0) records.splice(index, 1);
  records.push(record);
  records.sort((a: NativeFaultRecord, b: NativeFaultRecord) => a.timestamp - b.timestamp);
  while (records.length > MAX_RECORDS) { records.shift(); ++evictedRecords; }
}

async function Initialize(filesDir: string): Promise<void> {
  if (!filesDir.startsWith('/') || filesDir.includes('/../')) throw new Error('Invalid application files directory');
  if (filesDirectory !== '' && filesDirectory !== filesDir) throw new Error('Crash diagnostics files directory changed');
  filesDirectory = filesDir;
  if (loaded) return;
  loaded = true;
  const path = filesDirectory + '/' + FILE_NAME;
  try {
    if (!await fileIo.access(path)) return;
    const info = await fileIo.stat(path);
    if (info.size > MAX_ARCHIVE_BYTES) throw new Error('Existing fault archive exceeds byte limit');
    const previous = JSON.parse(await fileIo.readText(path, { length: MAX_ARCHIVE_BYTES })) as NativeFaultArchive;
    if (previous.schemaVersion !== 1 || previous.selfBundle !== SELF_BUNDLE || !Array.isArray(previous.records))
      throw new Error('Existing fault archive has an unsupported schema or bundle');
    evictedRecords = typeof previous.evictedRecords === 'number' ? previous.evictedRecords : 0;
    for (const record of previous.records.slice(-MAX_RECORDS)) {
      if (typeof record.key !== 'string' || typeof record.fullLog !== 'string' ||
          typeof record.timestamp !== 'number' || !Array.isArray(record.externalLogs))
        throw new Error('Existing fault archive contains a malformed record');
      for (const log of record.externalLogs) {
        if (!log || typeof log.complete !== 'boolean' || typeof log.fullLog !== 'string')
          throw new Error('Existing fault archive contains a malformed external log');
      }
      Retain(record);
    }
  } catch (error) { archiveReadError = ErrorText(error); }
}

function Archive(): NativeFaultArchive {
  return {
    schemaVersion: 1, selfBundle: SELF_BUNDLE, capturedAt: new Date().toISOString(),
    recordLimit: MAX_RECORDS, maximumLogBytes: MAX_LOG_BYTES, maximumArchiveBytes: MAX_ARCHIVE_BYTES,
    evictedRecords: evictedRecords, watcherRegistered: watcherRegistered,
    historicalQueryDeprecatedSince: 18, historicalQueries: historicalQueries,
    watcherError: watcherError, queryError: queryError, archiveReadError: archiveReadError, records: records
  };
}

async function Save(): Promise<void> {
  saveError = '';
  try {
    let bytes = encoder.encodeInto(JSON.stringify(Archive()));
    while (bytes.byteLength > MAX_ARCHIVE_BYTES && records.length > 0) {
      records.shift(); ++evictedRecords;
      bytes = encoder.encodeInto(JSON.stringify(Archive()));
    }
    if (bytes.byteLength > MAX_ARCHIVE_BYTES) throw new Error('Fault archive metadata exceeds byte limit');
    const temporary = filesDirectory + '/' + FILE_NAME + '.tmp';
    const output = await fileIo.open(temporary, fileIo.OpenMode.CREATE | fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.TRUNC);
    try {
      let offset: number = 0;
      while (offset < bytes.byteLength) {
        const chunk = bytes.buffer.slice(bytes.byteOffset + offset, bytes.byteOffset + Math.min(offset + 65536, bytes.byteLength));
        const written = await fileIo.write(output.fd, chunk);
        if (written <= 0 || written > chunk.byteLength) throw new Error('Fault archive write made invalid progress');
        offset += written;
      }
      await fileIo.fsync(output.fd);
    } finally { await fileIo.close(output); }
    const result = await fileIo.stat(temporary);
    if (result.size !== bytes.byteLength) throw new Error('Fault archive byte count differs after write');
    await fileIo.rename(temporary, filesDirectory + '/' + FILE_NAME);
    savedAt = new Date().toISOString(); savedBytes = bytes.byteLength;
    hilog.info(0x4D47, 'NGFault', 'Saved own fault archive records=%{public}d bytes=%{public}d', records.length, savedBytes);
  } catch (error) {
    saveError = ErrorText(error);
    hilog.error(0x4D47, 'NGFault', 'Own fault archive save failed: %{public}s', saveError);
  }
}

function QueryCachedFaults(): Promise<FaultLogger.FaultLogInfo[]> {
  return new Promise<FaultLogger.FaultLogInfo[]>((resolve, reject): void => {
    let settled: boolean = false;
    const timeout = setTimeout((): void => {
      if (settled) return;
      settled = true;
      reject(new Error('Historical FaultLogger query exceeded 5000ms; watcher collection remains enabled'));
    }, 5000);
    try {
      FaultLogger.query(FaultLogger.FaultType.CPP_CRASH).then((value: FaultLogger.FaultLogInfo[]): void => {
        if (settled) return;
        settled = true; clearTimeout(timeout); resolve(value);
      }, (error: Error): void => {
        if (settled) return;
        settled = true; clearTimeout(timeout); reject(error);
      });
    } catch (error) {
      settled = true; clearTimeout(timeout); reject(error);
    }
  });
}

async function QueryHistory(): Promise<void> {
  historyQueried = true; ++historicalQueries; queryError = '';
  try {
    // This deprecated API is only an explicit, bounded own-application cache
    // compatibility query. Future crash delivery uses the watcher below.
    const faults = await QueryCachedFaults();
    if (!Array.isArray(faults)) throw new Error('FaultLogger returned no fault array');
    for (const fault of faults.slice(0, MAX_RECORDS)) {
      if (fault.type !== FaultLogger.FaultType.CPP_CRASH) continue;
      const full = typeof fault.fullLog === 'string' ? fault.fullLog : '';
      const size = full.length > MAX_LOG_BYTES ? -1 : encoder.encodeInto(full).byteLength;
      const complete = size >= 0 && size <= MAX_LOG_BYTES && full.length > 0;
      const record: NativeFaultRecord = {
        key: 'FaultLogger:' + fault.timestamp + ':' + fault.pid + ':' + fault.type,
        source: 'FaultLogger.query(CPP_CRASH):self_bundle_and_uid', capturedAt: new Date().toISOString(),
        timestamp: fault.timestamp, pid: fault.pid, uid: fault.uid, crashType: 'NativeCrash',
        reason: fault.reason.slice(0, 8192), module: fault.module.slice(0, 2048), summary: fault.summary.slice(0, 256 * 1024),
        fullLog: complete ? full : '', fullLogBytes: size, fullLogComplete: complete,
        paramsJson: '', paramsComplete: true, logOverLimit: false, externalLogs: [],
        error: complete ? '' : full.length === 0 ? 'Historical fullLog is empty' : 'Historical fullLog exceeds byte limit; not silently truncated'
      };
      Retain(record);
    }
  } catch (error) { queryError = ErrorText(error); }
}

async function ReadExternal(path: string): Promise<ExternalCrashLog> {
  const output: ExternalCrashLog = { name: path.slice(path.lastIndexOf('/') + 1), fileBytes: 0, fullLog: '', complete: false, error: '' };
  try {
    // Only paths supplied by an own APP_CRASH event, inside known application
    // sandbox locations. Never enumerate a system fault directory.
    if (path.includes('/../') || !(path.startsWith('/data/storage/el2/log/hiappevent/') ||
        path.startsWith('/data/storage/el1/log/hiappevent/') || path.startsWith(filesDirectory + '/')))
      throw new Error('APP_CRASH external log is outside supported own-app locations');
    const info = await fileIo.stat(path); output.fileBytes = info.size;
    if (info.size > MAX_LOG_BYTES) throw new Error('External crash log exceeds byte limit; not silently truncated');
    const full = await fileIo.readText(path, { length: MAX_LOG_BYTES + 1 });
    if (encoder.encodeInto(full).byteLength !== info.size) throw new Error('External crash log changed or was not read completely');
    output.fullLog = full; output.complete = full.length > 0;
    if (!output.complete) output.error = 'External crash log is empty';
  } catch (error) { output.error = ErrorText(error); }
  return output;
}

async function Receive(groups: hiAppEvent.AppEventGroup[]): Promise<void> {
  let count: number = 0;
  for (const group of groups) {
    for (const event of group.appEventInfos) {
      if (event.domain !== hiAppEvent.domain.OS || event.name !== hiAppEvent.event.APP_CRASH) continue;
      const params = event.params as CrashEventParams;
      if (params.bundle_name !== undefined && params.bundle_name !== SELF_BUNDLE) continue;
      if (++count > MAX_RECORDS) break;
      const text = JSON.stringify(event.params);
      const timestamp = typeof params.time === 'number' ? params.time : Date.now();
      const pid = typeof params.pid === 'number' ? params.pid : 0;
      const external: ExternalCrashLog[] = [];
      const paths = params.external_log;
      if (Array.isArray(paths)) for (const path of paths.slice(0, 2)) external.push(await ReadExternal(path));
      const record: NativeFaultRecord = {
        key: 'hiAppEvent:' + (params.uuid === undefined ? timestamp + ':' + pid : params.uuid),
        source: 'hiAppEvent.APP_CRASH:own_application_watcher', capturedAt: new Date().toISOString(),
        timestamp: timestamp, pid: pid, uid: params.uid === undefined ? 0 : params.uid,
        crashType: params.crash_type === undefined ? 'unspecified' : params.crash_type,
        reason: '', module: '', summary: '', fullLog: '', fullLogBytes: 0, fullLogComplete: false,
        paramsJson: text.length <= MAX_EVENT_PARAMS_CHARS ? text : '', paramsComplete: text.length <= MAX_EVENT_PARAMS_CHARS,
        logOverLimit: params.log_over_limit === true, externalLogs: external,
        error: text.length <= MAX_EVENT_PARAMS_CHARS ? '' : 'Crash event metadata exceeds character limit; not silently truncated'
      };
      Retain(record);
    }
  }
  await Save();
}

async function Queue(action: () => Promise<void>): Promise<string> {
  const work = queueTail.then(async (): Promise<void> => {
    try { await action(); }
    catch (error) {
      saveError = ErrorText(error);
      hilog.error(0x4D47, 'NGFault', 'Own fault diagnostics failed: %{public}s', saveError);
    }
  });
  queueTail = work;
  await work;
  return GetCrashDiagnosticsStatus();
}

function EnsureWatcher(): void {
  if (watcherRegistered) return;
  watcherError = '';
  try {
    watcherHolder = hiAppEvent.addWatcher({
      name: 'nativeCrash', appEventFilters: [{ domain: hiAppEvent.domain.OS, names: [hiAppEvent.event.APP_CRASH] }],
      onReceive: (domain: string, groups: hiAppEvent.AppEventGroup[]): void => {
        if (domain === hiAppEvent.domain.OS) Queue(async (): Promise<void> => { await Receive(groups); });
      }
    });
    if (!watcherHolder) throw new Error('hiAppEvent.addWatcher returned no holder');
    watcherRegistered = true;
  } catch (error) { watcherError = ErrorText(error); }
}

export function StartCrashDiagnostics(filesDir: string): Promise<string> {
  return Queue(async (): Promise<void> => {
    await Initialize(filesDir); EnsureWatcher();
    if (!historyQueried) await QueryHistory();
    await Save();
  });
}

export function CollectCrashDiagnostics(filesDir: string): Promise<string> {
  return Queue(async (): Promise<void> => {
    await Initialize(filesDir); EnsureWatcher(); await QueryHistory(); await Save();
  });
}

export function GetCrashDiagnosticsStatus(): string {
  const fullLogs = records.filter((record: NativeFaultRecord) => record.fullLogComplete ||
    record.externalLogs.some((log: ExternalCrashLog) => log.complete)).length;
  return JSON.stringify({
    schemaVersion: 1, fileName: FILE_NAME, selfBundle: SELF_BUNDLE, watcherRegistered: watcherRegistered,
    historicalQueries: historicalQueries, recordCount: records.length, recordsWithCompleteLog: fullLogs,
    evictedRecords: evictedRecords, savedAt: savedAt, savedBytes: savedBytes,
    saveSucceeded: saveError.length === 0 && savedAt.length > 0, saveError: saveError,
    watcherError: watcherError, queryError: queryError, archiveReadError: archiveReadError
  });
}
