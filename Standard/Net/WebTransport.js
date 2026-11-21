// Emscripten JS library. A record holds one pending write and one input chunk.
// Duck owns the public handles. Promise continuations check local close before
// accessing registrations, which may have been destroyed inside a callback.
addToLibrary({
  $DuckWT__deps: ['$UTF16ToString', '$safeSetTimeout', 'malloc', 'calloc', 'free'],
  $DuckWT__postset: 'DuckWT.records = new Map();',
  $DuckWT: {
    next: 1,
    record(parent) {
      if (DuckWT.next > 0x7fffffff) return null;
      const r = {id: DuckWT.next++, parent, state: 0, children: new Set(),
                 watch: [], registered: [], offset: 0, input: null, terminal: null, openResult: 0};
      DuckWT.records.set(r.id, r);
      if (parent) parent.children.add(r);
      return r;
    },

    drop(r, kind) {
      const h = r.watch[kind];
      if (!h) return;
      r.watch[kind] = null;
      {{{ makeDynCall('vp', 'h.drop') }}}(h.context);
      _free(h.context);
      {{{ runtimeKeepalivePop() }}}
    },

    dispatch(r, kind, bytes, status) {
      const h = r.watch[kind];
      const data = bytes?.length ? _malloc(bytes.length) : 0;
      try {
        if (bytes?.length) HEAPU8.set(bytes, data);
        {{{ makeDynCall('vpipi', 'h.dispatch') }}}(h.context, kind, data, status);
      } finally {
        if (data) _free(data);
      }
    },

    notify(r) {
      if (r.closed || r.scheduled) return;
      const connected = r.watch[0] && r.openResult !== 0;
      const readable = r.watch[1] && (r.input !== null || r.terminal !== null);
      if (!connected && !readable) return;
      r.scheduled = true;
      safeSetTimeout(() => {
        r.scheduled = false;
        if (r.closed) return;
        if (r.watch[0] && r.openResult !== 0) {
          try { DuckWT.dispatch(r, 0, null, r.openResult); }
          finally { DuckWT.drop(r, 0); }
        }
        if (r.closed || !r.watch[1]) return;
        if (r.input !== null) {
          const count = r.parent ? Math.min(4096, r.input.length - r.offset) : r.input.length;
          const bytes = r.input.subarray(r.offset, r.offset + count);
          DuckWT.consume(r, count);
          DuckWT.dispatch(r, 1, bytes, count);
          DuckWT.notify(r);
        } else if (r.terminal !== null) {
          try { DuckWT.dispatch(r, 1, null, r.terminal); }
          finally { DuckWT.drop(r, 1); }
        }
      }, 0);
    },

    consume(r, count) {
      r.offset += count;
      if (r.offset === r.input.length) {
        r.input = null;
        r.offset = 0;
        DuckWT.pull(r);
      }
    },

    pull(r) {
      if (!r.reader || r.closed || r.reading || r.input !== null || r.terminal !== null) return;
      r.reading = true;
      // BYOB bounds stream reads independently of the sender's write sizes.
      const read = r.parent ? r.reader.read(new Uint8Array(4096)) : r.reader.read();
      read.then(({done, value}) => {
        r.reading = false;
        if (r.closed) return;
        if (done) r.terminal = r.parent ? 0 : -1;
        else if (!r.parent && value.byteLength > 65535) {
          r.terminal = -1;
          r.reader.cancel().catch(() => {});
        } else if (r.parent && !value.length) {
          DuckWT.pull(r);
        } else {
          r.input = value;
        }
        DuckWT.notify(r);
      }, () => {
        r.reading = false;
        if (!r.closed) { r.terminal = -1; DuckWT.notify(r); }
      });
    },

    attach(r, readable, writable) {
      r.reader = r.parent ? readable.getReader({mode: 'byob'}) : readable.getReader();
      r.writer = writable.getWriter();
      r.writer.closed.catch(() => { r.writeFailed = true; });
      r.state = r.openResult = 1;
      DuckWT.pull(r);
      DuckWT.notify(r);
    },

    ended(r) {
      if (r.closed || r.state === -1) return;
      r.state = -1;
      if (!r.openResult) r.openResult = -1;
      if (r.openResult < 0 || !r.reader) r.terminal = -1;
      DuckWT.notify(r);
      if (!r.parent) {
        for (const child of r.children) DuckWT.ended(child);
        DuckWT.releaseSession(r);
      }
    },

    releaseSession(r) {
      if (!r.alive) return;
      r.alive = false;
      // A scheduled callback checks for program exit after its keepalive pops.
      safeSetTimeout(() => {}, 0);
      {{{ runtimeKeepalivePop() }}}
    },

    close(r) {
      if (!r || r.closed) return;
      r.closed = true;
      DuckWT.records.delete(r.id);
      if (r.parent) r.parent.children.delete(r);
      for (const child of r.children) DuckWT.close(child);
      r.input = null;
      DuckWT.drop(r, 0);
      DuckWT.drop(r, 1);
      if (r.reader) r.reader.cancel().catch(() => {});
      if (r.writer && !r.finishing) r.writer.abort().catch(() => {});
      if (!r.parent) {
        r.transport.close();
        DuckWT.releaseSession(r);
      }
    },
  },

  duck_wt_connect__deps: ['$DuckWT'],
  duck_wt_connect__sig: 'ip',
  duck_wt_connect: (url) => {
    let transport;
    try { transport = new WebTransport(UTF16ToString(url), {requireUnreliable: true}); }
    catch { return 0; }
    const r = DuckWT.record(null);
    if (!r) { transport.closed.catch(() => {}); transport.ready.catch(() => {}); transport.close(); return 0; }
    r.transport = transport;
    r.alive = true;
    {{{ runtimeKeepalivePush() }}}
    transport.ready.then(() => {
      if (!r.closed && r.state === 0) DuckWT.attach(r, transport.datagrams.readable, transport.datagrams.writable);
    }).catch(() => DuckWT.ended(r));
    transport.closed.then(() => DuckWT.ended(r), () => DuckWT.ended(r));
    return r.id;
  },

  duck_wt_open__deps: ['$DuckWT'],
  duck_wt_open: (id) => {
    const parent = DuckWT.records.get(id);
    if (!parent || parent.parent || parent.state < 0) return 0;
    const r = DuckWT.record(parent);
    if (!r) return 0;
    parent.transport.ready.then(async () => {
      if (r.closed || r.state < 0) return;
      const stream = await parent.transport.createBidirectionalStream();
      if (r.closed || r.state < 0) {
        stream.readable.cancel().catch(() => {});
        stream.writable.abort().catch(() => {});
      } else DuckWT.attach(r, stream.readable, stream.writable);
    }).catch(() => DuckWT.ended(r));
    return r.id;
  },

  duck_wt_status__deps: ['$DuckWT'],
  duck_wt_status: (id) => DuckWT.records.get(id)?.state ?? -1,
  duck_wt_close__deps: ['$DuckWT'],
  duck_wt_close: (id) => DuckWT.close(DuckWT.records.get(id)),
  duck_wt_limit__deps: ['$DuckWT'],
  duck_wt_limit: (id) => {
    const r = DuckWT.records.get(id);
    return r && r.state === 1 && !r.parent ? Math.min(65535, r.transport.datagrams.maxDatagramSize) : 0;
  },

  duck_wt_scratch__sig: 'pi',
  duck_wt_scratch__deps: ['malloc'],
  duck_wt_scratch: (size) => _malloc(Math.max(1, size)),
  duck_wt_release__sig: 'vp',
  duck_wt_release__deps: ['free'],
  duck_wt_release: (ptr) => _free(ptr),

  duck_wt_transfer__deps: ['$DuckWT'],
  duck_wt_transfer__sig: 'iipii',
  duck_wt_transfer: (id, ptr, size, writing) => {
    const r = DuckWT.records.get(id);
    if (!r || size < 0) return -1;
    if (!writing) {
      if (r.registered[1]) return -1;
      if (r.input === null) return r.terminal ?? -2;
      const available = r.input.length - r.offset;
      if (!r.parent && size < available) { DuckWT.consume(r, available); return -3; }
      const count = Math.min(available, size);
      HEAPU8.set(r.input.subarray(r.offset, r.offset + count), ptr);
      DuckWT.consume(r, count);
      return count;
    }
    if (r.state < 0 || r.writeFailed || r.finishing) return -1;
    if (!r.writer || r.writing) return -2;
    if (!r.parent && size > r.transport.datagrams.maxDatagramSize) return -3;
    r.writing = true;
    r.writer.write(HEAPU8.slice(ptr, ptr + size)).then(
      () => { r.writing = false; },
      () => { r.writing = false; r.writeFailed = true; }
    );
    return size;
  },

  duck_wt_finish__deps: ['$DuckWT'],
  duck_wt_finish: (id) => {
    const r = DuckWT.records.get(id);
    if (!r?.parent || !r.writer || r.state !== 1 || r.writeFailed || r.finishing) return 0;
    r.finishing = true;
    r.writer.close().catch(() => { r.writeFailed = true; });
    return 1;
  },

  duck_wt_watch__deps: ['$DuckWT'],
  duck_wt_watch__sig: 'iiipppp',
  duck_wt_watch: (id, kind, source, store, dispatch, drop) => {
    const r = DuckWT.records.get(id);
    if (!r || r.registered[kind]) return 0;
    // DuckWebHandler has two managed func fields. Duck copies and drops them.
    const context = _calloc(2, {{{ POINTER_SIZE }}});
    if (!context) return 0;
    {{{ makeDynCall('vpp', 'store') }}}(context, source);
    // A remote session close can precede the last readable-stream callback.
    // Keep Wasm callable until this particular registration has been dropped.
    {{{ runtimeKeepalivePush() }}}
    r.watch[kind] = {context, dispatch, drop};
    r.registered[kind] = true;
    DuckWT.notify(r);
    return 1;
  },
});
