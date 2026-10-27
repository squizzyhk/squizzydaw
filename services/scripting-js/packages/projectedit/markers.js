// Markers.
//   Insert Marker      args: position (quarter notes, required), name?, length?
//   Markers From Clips args: selection; one marker per clip start, named after the clip
function createMarkerCommands() {
  return Host.Commands.handler([
    {
      category: 'Project', name: 'Insert Marker', title: 'Insert Marker',
      enabled: (ctx) => typeof ctx.args.position === 'number' && ctx.args.position >= 0,
      run(ctx) {
        const name = String(ctx.arg('name', 'Marker ' + (ctx.doc.markers.length + 1)));
        ctx.doc.execute(Host.Actions.AddMarker, {
          position: Host.Values.double(ctx.args.position),
          length: Host.Values.double(Math.max(0, Number(ctx.arg('length', 0)))),
          name: Host.Values.string(name),
        });
      },
    },
    {
      category: 'Project', name: 'Markers From Clips', title: 'Add Markers at Clip Starts',
      enabled: (ctx) => ctx.selectedClips().length > 0,
      run(ctx) {
        const existing = new Set(ctx.doc.markers.map((m) => m.position));
        let added = 0;
        for (const clip of ctx.selectedClips()) {
          const pos = clip.start;
          if (existing.has(pos)) continue;
          existing.add(pos);
          ctx.doc.execute(Host.Actions.AddMarker, { position: Host.Values.double(pos), name: Host.Values.string(clip.name || 'Clip') });
          ++added;
        }
        return added > 0;
      },
    },
  ]);
}
