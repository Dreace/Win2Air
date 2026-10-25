using System.Globalization;
using System.Runtime.InteropServices.WindowsRuntime;
using LiveChartsCore;
using LiveChartsCore.Defaults;
using LiveChartsCore.SkiaSharpView;
using LiveChartsCore.SkiaSharpView.Painting;
using Microsoft.UI.Xaml.Controls;
using SkiaSharp;

namespace Win2Air.Charts;

public sealed partial class TelemetryChart : UserControl
{
    private readonly LineSeries<ObservablePoint> _series = new()
    {
        GeometrySize = 0,
        LineSmoothness = 0,
        Fill = null
    };
    private readonly Axis _timeAxis = new() { TextSize = 12 };
    private readonly Axis _valueAxis = new() { MinLimit = 0, TextSize = 12 };
    private ObservablePoint[] _points = [];
    private int _windowSeconds;
    private int _metric = -1;
    private bool? _dark;

    public TelemetryChart()
    {
        InitializeComponent();
        Chart.AnimationsSpeed = TimeSpan.Zero;
        Chart.Series = new ISeries[] { _series };
        Chart.XAxes = new[] { _timeAxis };
        Chart.YAxes = new[] { _valueAxis };
    }

    // Native callers provide bounded, already aggregated samples. NaN marks a
    // missing interval so disconnections never appear as zero traffic.
    public void Update([ReadOnlyArray] double[] ageSeconds,
                       [ReadOnlyArray] double[] values, int windowSeconds,
                       int metric, string label, bool dark)
    {
        if (!DispatcherQueue.HasThreadAccess)
            throw new InvalidOperationException("Charts must update on the UI thread.");
        if (ageSeconds.Length != values.Length || ageSeconds.Length > 120 ||
            windowSeconds is not (60 or 600 or 3600) || metric is < 0 or > 4)
            throw new ArgumentException("Chart samples or metric are invalid.");

        for (var index = 0; index < values.Length; ++index)
        {
            if (!double.IsFinite(ageSeconds[index]) || ageSeconds[index] < -windowSeconds ||
                ageSeconds[index] > 0 || double.IsInfinity(values[index]) || values[index] < 0)
                throw new ArgumentException("Chart sample is outside the visible range.");
        }

        // Keep chart entities alive across samples. Replacing them on every tick
        // also replaces their drawing state, even for unchanged aligned buckets.
        if (_points.Length != values.Length)
        {
            _points = new ObservablePoint[values.Length];
            for (var index = 0; index < values.Length; ++index)
                _points[index] = new ObservablePoint();
            _series.Values = _points;
        }
        for (var index = 0; index < values.Length; ++index)
        {
            var point = _points[index];
            double? value = double.IsNaN(values[index]) ? null : values[index];
            if (point.X != ageSeconds[index]) point.X = ageSeconds[index];
            if (point.Y != value) point.Y = value;
        }
        _series.Name = label;

        if (_dark != dark)
        {
            _dark = dark;
            var foreground = new SKColor(dark ? 0xffc5c5c5u : 0xff606060u);
            var grid = new SKColor(dark ? 0xff454545u : 0xffd9d9d9u);
            var line = new SKColor(dark ? 0xffe0e0e0u : 0xff545454u);
            _series.Stroke = new SolidColorPaint(line, 2);
            _timeAxis.LabelsPaint = new SolidColorPaint(foreground);
            _timeAxis.SeparatorsPaint = null;
            _valueAxis.LabelsPaint = new SolidColorPaint(foreground);
            _valueAxis.SeparatorsPaint = new SolidColorPaint(grid);
        }
        if (_windowSeconds != windowSeconds)
        {
            _windowSeconds = windowSeconds;
            _timeAxis.MinLimit = -windowSeconds;
            _timeAxis.MaxLimit = 0;
            _timeAxis.MinStep = windowSeconds / 6.0;
            _timeAxis.Labeler = age => windowSeconds > 60 && Math.Abs(age) >= 60
                ? $"{Math.Abs(age) / 60:0.#} min" : $"{Math.Abs(age):0} s";
        }
        if (_metric == metric) return;
        _metric = metric;
        string Format(double value) => metric switch
        {
            0 => value >= 1_000_000 ? $"{value / 1_000_000:0.#} MB" :
                 value >= 1_000 ? $"{value / 1_000:0.#} kB" : $"{value:0} B",
            2 => $"{value:0.#} ms",
            3 => value >= 1_000_000 ? $"{value / 1_000_000:0.#} Mbps" : $"{value / 1_000:0.#} kbps",
            4 => $"{value:0.#} /s",
            _ => value.ToString("N0", CultureInfo.CurrentCulture)
        };

        _series.YToolTipLabelFormatter = point => Format(point.Coordinate.PrimaryValue);
        _valueAxis.Labeler = Format;
    }
}
