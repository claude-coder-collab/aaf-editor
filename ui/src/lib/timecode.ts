/// Formats a frame count as HH:MM:SS:FF (or HH:MM:SS;FF for drop-frame), SMPTE 12M style.
export function formatTimecode(frames: number, fps: number, drop = false): string {
  if (!Number.isFinite(frames) || fps <= 0) {
    return "--:--:--:--";
  }
  const negative = frames < 0;
  let f = Math.floor(Math.abs(frames));
  const nominal = Math.round(fps);
  if (drop && (nominal === 30 || nominal === 60)) {
    const dropped = nominal === 30 ? 2 : 4;
    const perMinute = nominal * 60 - dropped;
    const perTenMinutes = perMinute * 10 + dropped;
    const tens = Math.floor(f / perTenMinutes);
    const rest = f % perTenMinutes;
    f += dropped * 9 * tens + (rest > dropped ? dropped * Math.floor((rest - dropped) / perMinute) : 0);
  }
  const ff = f % nominal;
  const totalSeconds = Math.floor(f / nominal);
  const ss = totalSeconds % 60;
  const mm = Math.floor(totalSeconds / 60) % 60;
  const hh = Math.floor(totalSeconds / 3600);
  const pad = (n: number) => String(n).padStart(2, "0");
  return `${negative ? "-" : ""}${pad(hh)}:${pad(mm)}:${pad(ss)}${drop ? ";" : ":"}${pad(ff)}`;
}

/// Parses HH:MM:SS:FF (or ;FF for drop-frame) back to a frame count; returns null if malformed.
export function parseTimecode(text: string, fps: number, drop = false): number | null {
  const match = /^(-)?(\d{1,2}):(\d{2}):(\d{2})[:;.](\d{2})$/.exec(text.trim());
  if (!match || fps <= 0) {
    return null;
  }
  const [, sign, h, m, s, f] = match;
  const nominal = Math.round(fps);
  const hours = Number(h);
  const minutes = Number(m);
  const seconds = Number(s);
  const frames = Number(f);
  if (minutes > 59 || seconds > 59 || frames >= nominal) {
    return null;
  }
  const totalMinutes = hours * 60 + minutes;
  let total = (totalMinutes * 60 + seconds) * nominal + frames;
  if (drop && (nominal === 30 || nominal === 60)) {
    const dropped = nominal === 30 ? 2 : 4;
    total -= dropped * (totalMinutes - Math.floor(totalMinutes / 10));
  }
  return sign ? -total : total;
}

/// Nominal frames per second of an edit rate given as a fraction.
export function nominalFps(num: number, den: number): number {
  return den > 0 ? Math.round(num / den) : 0;
}
