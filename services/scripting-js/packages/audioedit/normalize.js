// Normalize Clips: sets the clip gain of every selected audio clip so that its loudest sample
// lands on the target level. The peak is measured over the part of the source file the clip
// actually plays, through whichever codec service can read the file.
//   args: selection (clip or track ids), target (dBFS, default -1.0)
function createNormalizeCommand() {
  return Host.Commands.handler({
    category: 'Audio',
    name: 'Normalize',
    title: 'Normalize Clips',
    enabled: (ctx) => ctx.selectedClips().some((c) => c.isAudio),
    run(ctx) {
      const target = Number(ctx.arg('target', -1.0));
      const tempo = ctx.doc.tempoMap;
      let changed = 0;
      for (const clip of ctx.selectedClips()) {
        if (!clip.isAudio) continue;
        const info = clip.clipInfo;
        let duration;
        if (tempo) {
          const a = tempo.ppqToSeconds(info.start);
          const b = tempo.ppqToSeconds(info.start + info.length);
          if (b > a) duration = b - a;
        }
        const level = Host.Audio.peak(clip.sourcePath, info.sourceOffset, duration);
        if (!level || !(level.peak > 0)) {
          console.warn('normalize: nothing readable in', clip.name);
          continue;
        }
        const gain = Math.round((target - level.peakDb) * 100) / 100;
        ctx.doc.execute(Host.Actions.SetProperty, { node: clip.id, key: 'gainDb', value: Host.Values.double(gain) });
        ++changed;
      }
      return changed > 0;
    },
  });
}
