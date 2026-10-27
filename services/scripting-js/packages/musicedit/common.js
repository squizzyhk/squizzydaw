// Shared helpers of the musicedit package (loaded with Host.Package.include).
var MusicEdit = MusicEdit || {
  midiClips(ctx) {
    return ctx.selectedClips().filter((c) => c.isMidi);
  },
  // Replaces a clip's events if `edit(events)` returned true.
  rewrite(ctx, clip, edit) {
    const events = clip.events;
    if (!edit(events)) return false;
    events.sort((a, b) => a.position - b.position);
    ctx.doc.execute(Host.Actions.SetClipEvents, { clip: clip.id, events: Host.Midi.packEvents(events) });
    return true;
  },
};
