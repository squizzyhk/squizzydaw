// Squizzy Studio scripting host API. Evaluated once per script context (the global evaluate()
// context and one context per loaded package). It receives the native binding `N` and returns
// { wrap } so native code can hand typed wrappers to script callbacks.
//
// Value conventions (script <-> IAttributes):
//   integral number -> int, other number -> double, boolean -> bool, string -> string,
//   canonical "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" string -> guid, ArrayBuffer/typed array -> blob,
//   array of GUID strings -> blob (packed sq_guid[]), host wrapper -> object, plain object -> nested
//   attributes, object with methods -> script object. Host.Values.int/double/string/guid force a type.
(function (global, N, packageInfo) {
  'use strict';

  const Results = Object.freeze({
    OK: 0, FALSE: 1, DISABLED: 1, FAIL: -1, INVALIDARG: -2, NOINTERFACE: -3, NOTFOUND: -4,
    OUTOFMEMORY: -5, BUSY: -7, TIMEOUT: -8, IO: -9, PARSE: -10, NOTIMPL: -11, CLOSED: -13,
    EXISTS: -14, ACCESS: -15, TYPE: -16, UNSUPPORTED: -17,
  });

  class HostError extends Error {
    constructor(message, code) {
      super(message);
      this.name = 'HostError';
      this.code = code;
    }
  }

  function check(code, what) {
    if (typeof code === 'number' && code < 0) {
      const nested = N.nestedError();
      throw new HostError(what + ' failed: ' + N.resultString(code) + (nested ? '\n  caused by: ' + nested : ''), code);
    }
    return code;
  }

  const Interfaces = Object.freeze(Object.assign({}, N.iid));

  const Values = Object.freeze({
    int: (v) => ({ __sqType: 'int', value: Math.trunc(Number(v)) }),
    double: (v) => ({ __sqType: 'double', value: Number(v) }),
    string: (v) => ({ __sqType: 'string', value: String(v) }),
    guid: (v) => ({ __sqType: 'guid', value: String(v) }),
  });

  // ---- wrappers for native objects ------------------------------------------------------
  function raw(x) {
    return x instanceof NativeObject ? x.handle : x;
  }

  class NativeObject {
    constructor(handle) {
      Object.defineProperty(this, '__sqHandle', { value: handle, enumerable: false });
    }
    get handle() { return this.__sqHandle; }
    supports(iid) { return N.qi(this.handle, iid); }
    equals(other) { return N.same(this.handle, raw(other)); }
    // IController
    get(name) { return wrap(N.ctlGet(this.handle, String(name))); }
    set(name, value) { check(N.ctlSet(this.handle, String(name), value), 'setProperty(' + name + ')'); }
    child(name) { return wrap(N.ctlChild(this.handle, String(name))); }
    // ICommandHandler
    interpretCommand(command, args, checkOnly) {
      return N.cmdInterpret(this.handle, String(command.category || ''), String(command.name || ''), args || null, !!checkOnly);
    }
    // IObserver
    onNotify(subject, topic, args) { return N.notify(this.handle, raw(subject) || null, String(topic), args || null); }
    // ISubject
    addObserver(observer) { return check(N.subjAdd(this.handle, raw(observer)), 'addObserver'); }
    removeObserver(observer) { return check(N.subjRemove(this.handle, raw(observer)), 'removeObserver'); }
    // IComponent
    initialize(context) { return check(N.compInit(this.handle, raw(context) || null), 'initialize'); }
    terminate() { return check(N.compTerm(this.handle), 'terminate'); }
    // IAction
    perform(doc, editor, params) {
      const r = N.actPerform(this.handle, raw(doc), raw(editor), params || null);
      check(r.code, 'perform');
      return r.result;
    }
  }

  const KIND_NAMES = { 1: 'Audio', 2: 'Instrument', 3: 'Bus', 4: 'FX', 5: 'Folder', 6: 'Master' };

  class Node extends NativeObject {
    get info() { return N.nodeInfo(this.handle); }
    get kind() { return this.info.kind; }
    get id() { return this.info.id; }
    get name() { return this.info.name; }
    get url() { return this.info.url; }
    get parent() { return wrap(N.nodeParent(this.handle)); }
    get children() {
      const n = this.info.childCount, out = [];
      for (let i = 0; i < n; ++i) out.push(wrap(N.nodeChild(this.handle, i)));
      return out;
    }
    properties() { return N.nodeProps(this.handle); }
    // Nodes are views: compare by persistent id, not by wrapper identity.
    equals(other) { return other instanceof Node && other.id === this.id; }
    toString() { return this.constructor.name + '(' + this.name + ')'; }
  }

  class Track extends Node {
    get trackInfo() { return N.trackInfo(this.handle); }
    get trackKind() { return this.trackInfo.kind; }
    get trackKindName() { return KIND_NAMES[this.trackInfo.kind] || 'Track'; }
    get clipCount() { return this.trackInfo.clipCount; }
    clip(i) { return wrap(N.trackClip(this.handle, i)); }
    get clips() {
      const n = this.clipCount, out = [];
      for (let i = 0; i < n; ++i) out.push(this.clip(i));
      return out;
    }
  }

  class Clip extends Node {
    get clipInfo() { return N.clipInfo(this.handle); }
    get start() { return this.clipInfo.start; }
    get length() { return this.clipInfo.length; }
    get end() { const c = this.clipInfo; return c.start + c.length; }
    get isAudio() { return this.clipInfo.kind === 1; }
    get isMidi() { return this.clipInfo.kind === 2; }
    get muted() { return (this.clipInfo.flags & 0x01) !== 0; }   // SQ_CLIP_MUTED
    get locked() { return (this.clipInfo.flags & 0x02) !== 0; }  // SQ_CLIP_LOCKED: position is fixed
    get sourcePath() { return N.clipSource(this.handle); }
    get events() { return N.clipEvents(this.handle); }
    get track() { return this.parent; }
  }

  class TempoMap extends Node {
    get tempos() { return N.tempoPoints(this.handle); }
    get signatures() { return N.tempoSignatures(this.handle); }
    ppqToSeconds(ppq) { return N.ppqToSeconds(this.handle, Number(ppq)); }
    secondsToPpq(sec) { return N.secondsToPpq(this.handle, Number(sec)); }
  }

  class Editor extends NativeObject {
    begin(description) { check(N.edBegin(this.handle, String(description || '')), 'beginTransaction'); }
    commit() { check(N.edCommit(this.handle), 'commitTransaction'); }
    cancel() { check(N.edCancel(this.handle), 'cancelTransaction'); }
    get inTransaction() { return N.edInTx(this.handle); }
    execute(action, params) {
      const r = N.edExecute(this.handle, String(action), params || null);
      check(r.code, 'execute(' + action + ')');
      return r.result;
    }
    // Runs fn inside one transaction (one undo step); cancels on any exception and rethrows.
    transaction(description, fn) {
      this.begin(description);
      let value;
      try {
        value = fn(this);
      } catch (e) {
        try { this.cancel(); } catch (_) { /* keep the original error */ }
        throw e;
      }
      this.commit();
      return value;
    }
    undo() { check(N.edUndo(this.handle), 'undo'); }
    redo() { check(N.edRedo(this.handle), 'redo'); }
    get canUndo() { return N.edCanUndo(this.handle); }
    get canRedo() { return N.edCanRedo(this.handle); }
    get undoCount() { return N.edUndoCount(this.handle); }
    get redoCount() { return N.edRedoCount(this.handle); }
    undoDescription(i) { return N.edUndoDesc(this.handle, i || 0); }
    redoDescription(i) { return N.edRedoDesc(this.handle, i || 0); }
    clearHistory() { check(N.edClearHistory(this.handle), 'clearHistory'); }
  }

  class Document extends Node {
    get documentInfo() { return N.docInfo(this.handle); }
    get path() { return N.docPath(this.handle); }
    get trackCount() { return this.documentInfo.trackCount; }
    track(i) { return wrap(N.docTrack(this.handle, i)); }
    get tracks() {
      const n = this.trackCount, out = [];
      for (let i = 0; i < n; ++i) out.push(this.track(i));
      return out;
    }
    get tempoMap() { return wrap(N.docTempoMap(this.handle)); }
    get markers() { return N.docMarkers(this.handle); }
    findNode(id) { return wrap(N.docFind(this.handle, String(id))); }
    // The engine's document object also answers IEditor, so wrap() would make it a Document again:
    // build the Editor view explicitly.
    get editor() { return new Editor(N.docEditor(this.handle)); }
    edit(description, fn) { return this.editor.transaction(description, fn); }
    execute(action, params) { return this.editor.execute(action, params); }
    // Editor API on the document itself, for native callers that box one object as both (e.g. an
    // IAction's perform(doc, editor) receiving the engine's combined document/editor object).
    begin(description) { this.editor.begin(description); }
    commit() { this.editor.commit(); }
    cancel() { this.editor.cancel(); }
    transaction(description, fn) { return this.editor.transaction(description, fn); }
    undo() { this.editor.undo(); }
    redo() { this.editor.redo(); }
    get canUndo() { return this.editor.canUndo; }
    get canRedo() { return this.editor.canRedo; }
    get undoCount() { return this.editor.undoCount; }
    get redoCount() { return this.editor.redoCount; }
    undoDescription(i) { return this.editor.undoDescription(i); }
    redoDescription(i) { return this.editor.redoDescription(i); }
  }

  class DocumentManager extends NativeObject {
    get active() { return wrap(N.mgrActive(this.handle)); }
    get documents() {
      const n = N.mgrCount(this.handle), out = [];
      for (let i = 0; i < n; ++i) out.push(wrap(N.mgrGet(this.handle, i)));
      return out;
    }
    findByUrl(url) { return wrap(N.mgrFindUrl(this.handle, String(url))); }
  }

  function wrap(v) {
    if (!N.isHandle(v)) return v === undefined ? null : v;
    if (N.qi(v, Interfaces.IDocument)) return new Document(v);
    if (N.qi(v, Interfaces.ITrack)) return new Track(v);
    if (N.qi(v, Interfaces.IClip)) return new Clip(v);
    if (N.qi(v, Interfaces.ITempoMap)) return new TempoMap(v);
    if (N.qi(v, Interfaces.INode)) return new Node(v);
    if (N.qi(v, Interfaces.IEditor)) return new Editor(v);
    if (N.qi(v, Interfaces.IDocumentManager)) return new DocumentManager(v);
    return new NativeObject(v);
  }

  // ---- Host ----------------------------------------------------------------------------
  const Classes = Object.freeze({
    createInstance(classId, iid) { return wrap(N.createInstance(String(classId), iid || Interfaces.IUnknown)); },
    find(classId) { return N.findClass(String(classId)); },
    list(category) {
      const n = N.classCount(), out = [];
      for (let i = 0; i < n; ++i) {
        const c = N.classInfo(i);
        if (c && (category === undefined || c.category === category)) out.push(c);
      }
      return out;
    },
  });

  const Objects = Object.freeze({
    getObjectByUrl(url, iid) { return wrap(N.resolve(String(url), iid || Interfaces.IUnknown)); },
    readText(url) { return N.readUrl(String(url)); },
    register(url, object) { return check(N.registerObject(String(url), raw(object)), 'registerObject(' + url + ')'); },
    unregister(url) { return N.unregisterObject(String(url)); },
  });

  const Settings = Object.freeze({
    get(key, def) { const v = N.settingsGet(String(key)); return v === undefined ? def : v; },
    set(key, value) { return check(N.settingsSet(String(key), value), 'settings.set(' + key + ')'); },
    has(key) { return N.settingsHas(String(key)); },
    remove(key) { return N.settingsRemove(String(key)); },
    keys(prefix) { return N.settingsKeys(String(prefix || '')); },
    // fn(key) on every change at/below prefix; returns a function that stops observing.
    observe(prefix, fn) {
      const id = N.settingsObserve(String(prefix || ''), fn);
      return () => N.settingsUnobserve(id);
    },
  });

  const Documents = Object.freeze({
    manager() { return wrap(N.queryService(Interfaces.IDocumentManager)); },
    active() { const m = this.manager(); return m ? m.active : null; },
    all() { const m = this.manager(); return m ? m.documents : []; },
    findByUrl(url) { const m = this.manager(); return m ? m.findByUrl(url) : null; },
  });

  const Actions = Object.freeze(Object.assign({}, N.actions));

  // ---- command helpers -----------------------------------------------------------------
  function parseSelection(doc, value) {
    let ids = [];
    if (value === undefined || value === null) ids = [];
    else if (typeof value === 'string') ids = value.split(/[;,\s]+/).filter((s) => s.length > 0);
    else if (value instanceof ArrayBuffer) ids = N.unpackGuids(value);
    else if (Array.isArray(value)) ids = value.map(String);
    const nodes = [];
    for (const id of ids) {
      const n = doc ? doc.findNode(id) : null;
      if (n) nodes.push(n);
    }
    return {
      nodes,
      clips: nodes.filter((n) => n instanceof Clip),
      tracks: nodes.filter((n) => n instanceof Track),
      get isEmpty() { return nodes.length === 0; },
    };
  }

  function makeContext(doc, command, args) {
    const a = args || {};
    return {
      doc,
      command,
      args: a,
      editor: doc ? doc.editor : null,
      selection: parseSelection(doc, a.selection),
      arg(name, def) { return a[name] === undefined || a[name] === null ? def : a[name]; },
      // Clips in the selection, plus every clip of each selected track.
      selectedClips() {
        const out = this.selection.clips.slice();
        for (const t of this.selection.tracks) for (const c of t.clips) out.push(c);
        return out;
      },
    };
  }

  const NOTHING_TO_DO = Object.freeze({ nothingToDo: true });

  // Builds an ICommandHandler script object from one spec or an array of specs:
  //   { category, name, title?, needsDocument? (default true), enabled?(ctx) -> bool, run(ctx) }
  // run() executes inside one transaction titled `title` (one undo step); an exception cancels it.
  function handler(specs) {
    const list = Array.isArray(specs) ? specs : [specs];
    return {
      interfaces: [Interfaces.ICommandHandler],
      commands: list.map((s) => ({ category: s.category, name: s.name, title: s.title || s.name })),
      interpretCommand(command, args, checkOnly) {
        const spec = list.find((s) => s.category === command.category && s.name === command.name);
        if (!spec) return Results.NOTFOUND;
        const doc = Documents.active();
        if (spec.needsDocument !== false && !doc) return Results.DISABLED;
        const ctx = makeContext(doc, command, args);
        if (spec.enabled && !spec.enabled(ctx)) return Results.DISABLED;
        if (checkOnly) return Results.OK;
        if (!doc || spec.needsDocument === false) return spec.run(ctx) === false ? Results.FALSE : Results.OK;
        // run() returning false means "nothing to do": the (empty) transaction is cancelled.
        let nothing = false;
        try {
          doc.edit(spec.title || spec.name, () => {
            if (spec.run(ctx) === false) {
              nothing = true;
              throw NOTHING_TO_DO;
            }
          });
        } catch (e) {
          if (e !== NOTHING_TO_DO) throw e;
        }
        return nothing ? Results.FALSE : Results.OK;
      },
    };
  }

  // Finds the first "Command" class that handles category/name and runs it.
  function invoke(category, name, args, checkOnly) {
    const command = { category: String(category), name: String(name), flags: 0 };
    for (const c of Classes.list('Command')) {
      const obj = Classes.createInstance(c.classId, Interfaces.ICommandHandler);
      if (!obj || typeof obj.interpretCommand !== 'function') continue;
      const r = obj.interpretCommand(command, args || null, true);
      if (r === Results.NOTFOUND) continue;
      if (r !== Results.OK && r !== undefined && r !== true) {
        if (checkOnly) return r;
        throw new HostError('command ' + category + '/' + name + ' is disabled', Results.DISABLED);
      }
      if (checkOnly) return Results.OK;
      const x = obj.interpretCommand(command, args || null, false);
      check(typeof x === 'number' ? x : Results.OK, 'command ' + category + '/' + name);
      return Results.OK;
    }
    if (checkOnly) return Results.NOTFOUND;
    throw new HostError('no handler for command ' + category + '/' + name, Results.NOTFOUND);
  }

  const Commands = Object.freeze({ handler, invoke, makeContext, parseSelection });

  const Audio = Object.freeze({
    // Peak of a media file range via the codec services: { peak, peakDb, frames, sampleRate, channels }.
    peak(path, startSeconds, durationSeconds) {
      return N.audioPeak(String(path), Number(startSeconds || 0), durationSeconds === undefined ? -1 : Number(durationSeconds));
    },
    dbToGain: (db) => Math.pow(10, db / 20),
    gainToDb: (g) => (g > 0 ? 20 * Math.log10(g) : -Infinity),
  });

  const Midi = Object.freeze({
    NOTE_ON: 0x90,
    isNote: (e) => (e.status & 0xf0) === 0x90 && e.length > 0,
    // [{position, length, status, data1, data2}] -> ArrayBuffer of sq_clip_event[] (kSetClipEvents).
    packEvents: (events) => N.packEvents(events),
  });

  function log() {
    N.log(1, Array.prototype.map.call(arguments, (a) => (typeof a === 'string' ? a : safeString(a))).join(' '));
  }
  function safeString(v) {
    try { return typeof v === 'object' ? JSON.stringify(v) : String(v); } catch (_) { return String(v); }
  }
  const console = Object.freeze({
    log, info: log,
    debug() { N.log(0, Array.prototype.join.call(arguments, ' ')); },
    warn() { N.log(2, Array.prototype.join.call(arguments, ' ')); },
    error() { N.log(3, Array.prototype.join.call(arguments, ' ')); },
  });

  const Package = packageInfo ? Object.freeze({
    id: packageInfo.id,
    name: packageInfo.name,
    version: packageInfo.version,
    readText(path) { return N.readPackageText(String(path)); },
    include(path) { return N.include(String(path)); },
    url(path) { return 'package://' + packageInfo.id + '/' + path; },
  }) : null;

  const Host = Object.freeze({
    version: N.engineInfo,
    Results, HostError, Interfaces, Values, Classes, Objects, Settings, Documents, Actions,
    Commands, Audio, Midi, Package, console,
    log,
    wrap,
    setTimeout(fn, ms) { return N.timerAdd(fn, Number(ms || 0), false); },
    setInterval(fn, ms) { return N.timerAdd(fn, Number(ms || 0), true); },
    clearTimeout(id) { N.timerCancel(id); },
    clearInterval(id) { N.timerCancel(id); },
  });

  Object.defineProperty(global, 'Host', { value: Host, enumerable: false, writable: false, configurable: false });
  Object.defineProperty(global, 'console', { value: console, enumerable: false, writable: true, configurable: true });
  global.setTimeout = Host.setTimeout;
  global.clearTimeout = Host.clearTimeout;
  global.setInterval = Host.setInterval;
  global.clearInterval = Host.clearInterval;

  return { wrap };
});
