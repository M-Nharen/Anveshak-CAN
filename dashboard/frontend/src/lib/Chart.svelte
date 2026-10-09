<script>
  // A small uPlot line chart: `times` in seconds, `series` = [{label, values, color}].
  import { onMount, onDestroy } from 'svelte';
  import uPlot from 'uplot';
  import 'uplot/dist/uPlot.min.css';

  export let times = [];
  export let series = [];
  export let height = 160;

  let el;
  let plot = null;
  let observer = null;

  function build() {
    const style = getComputedStyle(document.documentElement);
    const text = style.getPropertyValue('--muted').trim() || '#667085';
    const grid = style.getPropertyValue('--line').trim() || '#d9dee7';
    plot = new uPlot(
      {
        width: Math.max(el.clientWidth, 200),
        height,
        cursor: { drag: { x: false, y: false } },
        legend: { show: true },
        scales: { x: { time: false } },
        axes: [
          { stroke: text, grid: { stroke: grid }, ticks: { stroke: grid }, values: (u, v) => v.map((x) => `${Math.round(x)} s`) },
          { stroke: text, grid: { stroke: grid }, ticks: { stroke: grid }, size: 50 },
        ],
        series: [{}, ...series.map((s) => ({ label: s.label, stroke: s.color, width: 2 }))],
      },
      [times, ...series.map((s) => s.values)],
      el
    );
  }

  onMount(() => {
    build();
    observer = new ResizeObserver(() => plot && plot.setSize({ width: Math.max(el.clientWidth, 200), height }));
    observer.observe(el);
  });
  onDestroy(() => {
    if (observer) observer.disconnect();
    if (plot) plot.destroy();
  });

  $: if (plot) plot.setData([times, ...series.map((s) => s.values)]);
</script>

<div bind:this={el}></div>
