// Duplicates the selected tracks (mixer settings + clips) right below each original.
//   args: selection (track ids), suffix (default " copy")
function copyTrack(ctx, track, suffix) {
  const doc = ctx.doc;
  const info = track.trackInfo;
  const index = doc.tracks.findIndex((t) => t.equals(track));
  const created = doc.execute(Host.Actions.AddTrack, {
    kind: info.kind,
    name: Host.Values.string(track.name + suffix),
    index: index >= 0 ? index + 1 : doc.trackCount,
    channels: info.channels,
  });
  const copy = created.node;
  const set = (key, value) => doc.execute(Host.Actions.SetProperty, { node: copy, key, value });
  if (info.volumeDb !== 0) set('volumeDb', Host.Values.double(info.volumeDb));
  if (info.pan !== 0) set('pan', Host.Values.double(info.pan));
  if (info.color !== 0) set('color', Host.Values.int(info.color));
  for (const clip of track.clips) {
    const c = clip.clipInfo;
    const params = { track: copy, kind: c.kind, start: Host.Values.double(c.start), length: Host.Values.double(c.length) };
    if (clip.isAudio) {
      params.source = Host.Values.string(clip.sourcePath);
      params.source_offset = Host.Values.double(c.sourceOffset);
    }
    const node = doc.execute(Host.Actions.AddClip, params).node;
    const setClip = (key, value) => doc.execute(Host.Actions.SetProperty, { node, key, value });
    if (clip.name) setClip('name', Host.Values.string(clip.name));
    if (c.gainDb !== 0) setClip('gainDb', Host.Values.double(c.gainDb));
    if (c.fadeIn !== 0) setClip('fadeIn', Host.Values.double(c.fadeIn));
    if (c.fadeOut !== 0) setClip('fadeOut', Host.Values.double(c.fadeOut));
    if (c.flags & 0x01) setClip('muted', true);  // SQ_CLIP_MUTED
    if (c.flags & 0x02) setClip('locked', true); // SQ_CLIP_LOCKED
    if (clip.isMidi && c.eventCount > 0) {
      doc.execute(Host.Actions.SetClipEvents, { clip: node, events: Host.Midi.packEvents(clip.events) });
    }
  }
  return copy;
}

function createDuplicateCommand() {
  return Host.Commands.handler({
    category: 'Track',
    name: 'Duplicate',
    title: 'Duplicate Tracks',
    enabled: (ctx) => ctx.selection.tracks.length > 0,
    run(ctx) {
      const suffix = String(ctx.arg('suffix', ' copy'));
      // Bottom-up so the insert positions of the remaining originals stay valid.
      for (const t of ctx.selection.tracks.slice().reverse()) copyTrack(ctx, t, suffix);
    },
  });
}
