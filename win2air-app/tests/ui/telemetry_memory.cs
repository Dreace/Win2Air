using System.Diagnostics;
using System.Text.Json;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Win2Air.Charts;

internal static class TelemetryMemoryTest
{
    internal static string Report;
    internal static bool Navigation;

    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length is < 1 or > 2 || !Path.IsPathFullyQualified(args[0])) return 2;
        if (args.Length == 2 && args[1] != "--navigation") return 2;
        Navigation = args.Length == 2;
        Report = args[0];
        try
        {
            WinRT.ComWrappersSupport.InitializeComWrappers();
            Application.Start(_ => new ChartMemoryApplication());
            return Environment.ExitCode;
        }
        catch (Exception error)
        {
            File.WriteAllText(Report, JsonSerializer.Serialize(new { error = error.ToString() }));
            return 1;
        }
    }
}

// Register the same control metadata used by the native application host.
internal sealed class ChartMemoryApplication : Application, IXamlMetadataProvider
{
    private readonly Microsoft.UI.Xaml.XamlTypeInfo.XamlControlsXamlMetaDataProvider _metadata = new();
    private readonly Win2Air.Charts.Win2Air_Charts_XamlTypeInfo.XamlMetaDataProvider _charts = new();
    public IXamlType GetXamlType(Type type) => _metadata.GetXamlType(type) ?? _charts.GetXamlType(type);
    public IXamlType GetXamlType(string name) => _metadata.GetXamlType(name) ?? _charts.GetXamlType(name);
    public XmlnsDefinition[] GetXmlnsDefinitions() => _metadata.GetXmlnsDefinitions();

    private readonly double[] _ages = new double[60], _values = new double[60];
    private readonly List<object> _samples = [];
    private Window _window;
    private StackPanel _panel;
    private readonly ContentControl _host = new();
    private TelemetryChart _primary, _secondary;
    private DispatcherQueueTimer _timer;
    private int _tick;
    private int _loads, _unloads;
    private bool _ticking;
    private long _baselinePrivate, _baselineManaged, _unchangedAllocations;

    public ChartMemoryApplication()
    {
        UnhandledException += (_, error) => Finish(error.Exception);
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        Resources.MergedDictionaries.Add(new XamlControlsResources());
        _primary = new TelemetryChart { Height = 250 };
        _secondary = new TelemetryChart { Height = 250 };
        _primary.Loaded += (_, _) => ++_loads;
        _secondary.Loaded += (_, _) => ++_loads;
        _primary.Unloaded += (_, _) => ++_unloads;
        _secondary.Unloaded += (_, _) => ++_unloads;
        var panel = new StackPanel { Language = "zh-CN" };
        panel.Children.Add(_primary);
        panel.Children.Add(_secondary);
        _panel = panel;
        _host.Content = panel;
        _window = new Window { Content = _host };
        _window.Activate();
        _timer = DispatcherQueue.GetForCurrentThread().CreateTimer();
        _timer.Interval = TimeSpan.FromMilliseconds(20);
        _timer.Tick += Tick;
        _timer.Start();
    }

    private void Tick(DispatcherQueueTimer timer, object args)
    {
        if (_ticking) return;
        _ticking = true;
        try
        {
            if (TelemetryMemoryTest.Navigation)
            {
                _panel.Visibility = _tick % 2 == 0 ? Visibility.Collapsed : Visibility.Visible;
                _primary.Update([], [], 600, 3, "Traffic", false);
                _secondary.Update([], [], 600, 2, "Latency", false);
                ++_tick;
                if (_tick % 100 != 0) return;
                GC.Collect();
                GC.WaitForPendingFinalizers();
                GC.Collect();
                using var navigationProcess = Process.GetCurrentProcess();
                long navigationPrivate = navigationProcess.PrivateMemorySize64;
                long navigationManaged = GC.GetTotalMemory(false);
                _samples.Add(new { tick = _tick, privateBytes = navigationPrivate, managedBytes = navigationManaged });
                if (_tick == 200) { _baselinePrivate = navigationPrivate; _baselineManaged = navigationManaged; }
                if (_tick < 1000) return;
                if (_loads != 2 || _unloads != 0)
                    throw new InvalidOperationException("Page switching recreated chart rendering resources.");
                if (navigationPrivate - _baselinePrivate > 16 * 1024 * 1024 ||
                    navigationManaged - _baselineManaged > 4 * 1024 * 1024)
                    throw new InvalidOperationException("Chart navigation retains memory after warmup.");
                Finish(null);
                return;
            }
            // Exercise aligned unchanged buckets, changing data, gaps, range,
            // metric and theme changes with actual chart rendering between ticks.
            int range = _tick < 100 ? 60 : (_tick / 1000 % 2 == 0 ? 600 : 3600);
            int metric = _tick / 1200 % 5;
            for (int index = 0; index < _values.Length; ++index)
            {
                _ages[index] = -(59.5 - index) * range / 60;
                _values[index] = index == 20 ? double.NaN
                    : 90000 + 10000 * Math.Sin((_tick / 10 + index) / 6.0);
            }
            _primary.Update(_ages, _values, range, metric, "Traffic", _tick / 1500 % 2 == 1);
            _secondary.Update(_ages, _values, range, 2, "Latency", false);
            if (++_tick % 500 != 0) return;

            if (_tick == 1000)
            {
                // Identical ten-minute buckets should keep their drawing state.
                for (int index = 0; index < _ages.Length; ++index)
                    _ages[index] = -(59.5 - index) * 10;
                _primary.Update(_ages, _values, 600, 3, "Traffic", false);
                long before = GC.GetAllocatedBytesForCurrentThread();
                for (int index = 0; index < 10000; ++index)
                    _primary.Update(_ages, _values, 600, 3, "Traffic", false);
                _unchangedAllocations = GC.GetAllocatedBytesForCurrentThread() - before;
                if (_unchangedAllocations > 2 * 1024 * 1024)
                    throw new InvalidOperationException("Unchanged chart updates allocate repeatedly.");
                bool rejected = false;
                try { _primary.Update([0], [double.PositiveInfinity], 600, 3, "Traffic", false); }
                catch (ArgumentException) { rejected = true; }
                if (!rejected) throw new InvalidOperationException("Invalid chart values were accepted.");
            }

            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            using var process = Process.GetCurrentProcess();
            long privateBytes = process.PrivateMemorySize64, managedBytes = GC.GetTotalMemory(false);
            _samples.Add(new { tick = _tick, privateBytes, managedBytes });
            if (_tick == 1500) { _baselinePrivate = privateBytes; _baselineManaged = managedBytes; }
            if (_tick < 6000) return;
            if (privateBytes - _baselinePrivate > 16 * 1024 * 1024 ||
                managedBytes - _baselineManaged > 4 * 1024 * 1024)
                throw new InvalidOperationException("Chart refresh retains memory after warmup.");
            Finish(null);
        }
        catch (Exception error) { Finish(error); }
        finally { _ticking = false; }
    }

    private void Finish(Exception error)
    {
        _timer?.Stop();
        Environment.ExitCode = error is null ? 0 : 1;
        File.WriteAllText(TelemetryMemoryTest.Report, JsonSerializer.Serialize(new
        {
            status = error is null ? "passed" : "failed",
            error = error?.ToString(),
            unchangedUpdateAllocatedBytes = _unchangedAllocations,
            chartLoads = _loads,
            chartUnloads = _unloads,
            samples = _samples
        }));
        _window?.Close();
        Exit();
    }
}
