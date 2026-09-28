/// Snaps `value` to the nearest of `points` within `tolerance` (same units); returns `value` when none is close.
export function snap(value: number, points: readonly number[], tolerance: number): number {
  let best = value;
  let distance = tolerance;
  for (const p of points) {
    const d = Math.abs(p - value);
    if (d <= distance) {
      best = p;
      distance = d;
    }
  }
  return best;
}

/// Where a drag hits an item: near an edge (for trimming) or in the body (for moving).
export function dragZone(x: number, left: number, width: number, edgePixels = 6): "head" | "tail" | "body" {
  const edge = Math.min(edgePixels, width / 4);
  if (x - left <= edge) return "head";
  if (left + width - x <= edge) return "tail";
  return "body";
}
