# Chart guidelines

Use Chart.js for data visualization. Load from the local server inside HTML type visuals.

## Setup
```html
<script src="/js/vendor/chart.umd.js"></script>

<canvas id="myChart"></canvas>
<script>
const ctx = document.getElementById('myChart');
new Chart(ctx, {
   type: 'bar',
   data: { ... },
   options: {
      responsive: true,
      maintainAspectRatio: false,
      plugins: { legend: { position: 'top' } }
   }
});
</script>
```

## Sizing (IMPORTANT)
The visual is rendered in a frame with a fixed height. Always set
`maintainAspectRatio: false` and do NOT put a `height` or `max-height` on the
`<canvas>`. With `maintainAspectRatio: true` (the Chart.js default) plus a
`max-height`-only canvas, the chart resolves its height from a content-sized
container, collapses to 0×0, and renders as a black box. `responsive: true` +
`maintainAspectRatio: false` lets the chart fill the frame correctly.

## Theming
Read CSS variables and apply to Chart.js:
```javascript
const style = getComputedStyle(document.documentElement);
const textColor = style.getPropertyValue('--color-text-primary').trim();
const gridColor = style.getPropertyValue('--color-border').trim();

Chart.defaults.color = textColor;
Chart.defaults.borderColor = gridColor;
```

## Color Mapping for Datasets
Use color ramp 600 stops for dataset colors:
- Dataset 1: #534AB7 (purple-600)
- Dataset 2: #0F6E56 (teal-600)
- Dataset 3: #993C1D (coral-600)
- Dataset 4: #185FA5 (blue-600)

## Chart Type Selection
- Comparison across categories → bar (horizontal if long labels)
- Trend over time → line
- Part of whole → doughnut (not pie — doughnut is more readable)
- Correlation → scatter
- Multi-variable comparison → radar
