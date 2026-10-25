#include "AppHost.h"

namespace Win2Air {
winrt::fire_and_forget AppHost::CaptureReview() {
  auto lifetime = get_strong();
  refresh.Stop();
  winrt::apartment_context ui;
  auto report = ui_report;
  try {
    // Render the actual XAML tree in an isolated review session. This does
    // not synthesize a desktop screenshot or claim pointer/desktop
    // validation.
    struct Selection {
      wchar_t const* control;
      int index;
      std::string expected;
    };
    for (auto const& selection : {Selection{L"SettingsTheme", 2, "dark"},
                                  Selection{L"SettingsTheme", 0, "system"},
                                  Selection{L"SettingsLanguage", 2, "en-US"},
                                  Selection{L"SettingsLanguage", 1, "zh-CN"},
                                  Selection{L"LatencyMode", 0, "ultra"},
                                  Selection{L"LatencyMode", 1, "adaptive"},
                                  Selection{L"LatencyMode", 2, "balanced"}}) {
      bool latency = std::wstring_view(selection.control) == L"LatencyMode";
      auto content = latency ? streaming_settings->Content() : settings_page;
      auto control = content.FindName(selection.control)
                         .as<winrt::Microsoft::UI::Xaml::Controls::ComboBox>();
      auto revision = streaming->Snapshot().preferences.revision;
      control.SelectedIndex(-1);
      control.SelectedIndex(selection.index);
      auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      for (;;) {
        co_await winrt::resume_after(std::chrono::milliseconds(25));
        co_await ui;
        RenderShell();
        auto saved = settings->Load().settings;
        auto actual = latency ? saved.latency_mode
                      : std::wstring_view(selection.control) == L"SettingsTheme"
                          ? saved.theme
                          : saved.language;
        if (saved.revision > revision && actual == selection.expected &&
            !preferences->Pending() && !streaming_settings->Pending())
          break;
        if (std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error(
              "Dropdown selection did not persist automatically: " +
              winrt::to_string(selection.control));
      }
    }
    struct Frame {
      wchar_t const* name;
      AppPage page;
      int section;
      int width;
      int height;
      bool dark;
      bool lower{};
      bool editor{};
      int dialog{};
    };
    Frame frames[] = {
        {L"playback-light", AppPage::playback, 0, 1120, 780, false},
        {L"statistics-light", AppPage::statistics, 0, 1120, 780, false},
        {L"settings-general-light", AppPage::settings, 0, 1120, 780, false},
        {L"settings-audio-light", AppPage::settings, 1, 1120, 780, false},
        {L"settings-streaming-light", AppPage::settings, 2, 1120, 780, false},
        {L"settings-receivers-light", AppPage::devices, 3, 1120, 780, false},
        {L"settings-receivers-wide-light", AppPage::devices, 3, 1600, 900,
         false},
        {L"receiver-editor-light", AppPage::devices, 3, 1120, 780, false, true,
         true},
        {L"statistics-details-light", AppPage::statistics, 0, 1120, 780, false,
         true},
        {L"settings-general-lower-light", AppPage::settings, 0, 1120, 780,
         false, true},
        {L"settings-receivers-lower-light", AppPage::devices, 3, 1120, 780,
         false, true},
        {L"pairing-dialog-light", AppPage::playback, 0, 1120, 780, false, false,
         false, 1},
        {L"add-receiver-dialog-light", AppPage::playback, 0, 1120, 780, false,
         false, false, 2},
        {L"playback-compact-dark", AppPage::playback, 0, 900, 640, true},
        {L"statistics-compact-dark", AppPage::statistics, 0, 900, 640, true},
        {L"settings-compact-dark", AppPage::settings, 0, 900, 640, true},
        {L"statistics-details-compact-dark", AppPage::statistics, 0, 900, 640,
         true, true},
        {L"receivers-details-compact-dark", AppPage::devices, 3, 900, 640, true,
         true},
        {L"pairing-dialog-error-dark", AppPage::playback, 0, 900, 640, true,
         false, false, 3},
        {L"tray-dark", AppPage::playback, -1, 900, 640, true}};
    auto folder =
        co_await winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(
            report.parent_path().make_preferred().wstring());
    winrt::Windows::Data::Json::JsonArray frame_reports;
    bool english = false;
    for (auto const& frame : frames) {
      if (frame.dark && !english) {
        ConfigureResources("en-US");
        LoadPlaybackPages();
        BuildShell(packaged_application);
        RenderShell();
        english = true;
      }
      HWND owner{};
      winrt::check_hresult(
          window.as<IWindowNative>()->get_WindowHandle(&owner));
      auto dpi = GetDpiForWindow(owner);
      window.AppWindow().Resize(
          {MulDiv(frame.width, dpi, 96), MulDiv(frame.height, dpi, 96)});
      Navigate(frame.page);
      if (frame.page == AppPage::settings || frame.page == AppPage::devices)
        preferences->SelectSection(frame.section);
      if (frame.page == AppPage::devices) {
        auto root = devices->Content();
        for (auto name : {L"ManualReceiverOptions"})
          root.FindName(name)
              .as<winrt::Microsoft::UI::Xaml::Controls::Expander>()
              .IsExpanded(frame.lower && !frame.editor);
        auto rows = root.FindName(L"ReceiverCards")
                        .as<winrt::Microsoft::UI::Xaml::Controls::StackPanel>();
        if (rows.Children().Size())
          rows.Children()
              .GetAt(0)
              .as<winrt::Microsoft::UI::Xaml::FrameworkElement>()
              .FindName(L"ReceiverDetails")
              .as<winrt::Microsoft::UI::Xaml::Controls::Expander>()
              .IsExpanded(frame.editor);
      }
      shell.RequestedTheme(
          frame.dark ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark
                     : winrt::Microsoft::UI::Xaml::ElementTheme::Light);
      co_await winrt::resume_after(std::chrono::milliseconds(250));
      co_await ui;
      winrt::Microsoft::UI::Xaml::Controls::ScrollViewer scroller{nullptr};
      if (frame.page == AppPage::statistics)
        scroller =
            statistics_page.FindName(L"StatisticsScroll")
                .as<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>();
      else if (frame.page == AppPage::devices)
        scroller =
            devices->Content()
                .as<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>();
      else if (frame.page == AppPage::settings) {
        wchar_t const* sections[] = {L"GeneralPanel", L"AudioPanel",
                                     L"StreamingPanel"};
        if (frame.section >= 0 && frame.section < 3)
          scroller =
              settings_page.FindName(sections[frame.section])
                  .as<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>();
      }
      if (scroller)
        scroller.ChangeView(
            nullptr,
            winrt::box_value(frame.lower ? scroller.ScrollableHeight() : 0.0)
                .as<winrt::Windows::Foundation::IReference<double>>(),
            nullptr, true);
      auto capture = shell;
      winrt::Microsoft::UI::Xaml::Controls::ContentDialog review_dialog{
          nullptr};
      winrt::Microsoft::UI::Xaml::Controls::PasswordBox review_pin{nullptr};
      winrt::Microsoft::UI::Xaml::Controls::TextBox review_host{nullptr};
      auto submitted = std::make_shared<unsigned>(0);
      std::function<void(wchar_t const*)> invoke_dialog_button;
      if (frame.section == -1) {
        ShowFlyout();
        capture = flyout_content;
      }
      co_await winrt::resume_after(std::chrono::milliseconds(250));
      co_await ui;
      shell.UpdateLayout();
      capture.UpdateLayout();
      if (frame.page == AppPage::statistics) {
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        int metric = frame.dark ? 2 : frame.lower ? 1 : 3;
        int range = frame.dark ? 2 : frame.lower ? 1 : 0;
        statistics_page.FindName(L"HistoryMetric")
            .as<ComboBox>()
            .SelectedIndex(metric);
        statistics_page.FindName(L"HistoryWindow")
            .as<ComboBox>()
            .SelectedIndex(range);
        const int windows[] = {60, 600, 3600};
        statistics_page.FindName(L"AudioHistoryWindow")
            .as<ComboBox>()
            .SelectedIndex(range);
        statistics_page.FindName(L"AudioHistoryMetric")
            .as<ComboBox>()
            .SelectedIndex(2);
        std::vector<double> ages, values;
        for (int index = 0; index < 60; ++index) {
          ages.push_back(-(59.5 - index) * windows[range] / 60.0);
          values.push_back(metric == 2 ? 90 + 5 * std::sin(index / 6.0)
                                       : (metric == 1 ? 1250 : 60000) *
                                             (1 + 0.1 * std::sin(index / 3.0)));
        }
        statistics_page.FindName(L"HistoryChart")
            .as<ContentControl>()
            .Content()
            .as<winrt::Win2Air::Charts::TelemetryChart>()
            .Update(ages, values, windows[range], metric, L"Virtual receiver",
                    frame.dark);
        for (size_t index = 0; index < values.size(); ++index)
          values[index] = 90 + 5 * std::sin(index / 6.0);
        statistics_page.FindName(L"AudioHistoryChart")
            .as<ContentControl>()
            .Content()
            .as<winrt::Win2Air::Charts::TelemetryChart>()
            .Update(ages, values, windows[range], 2, L"Virtual receiver delay",
                    frame.dark);
        co_await winrt::resume_after(std::chrono::milliseconds(500));
        co_await ui;
      }
      if (frame.dialog) {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        auto result = [submitted] {
          std::promise<CommandResult> completed;
          CommandResult value;
          value.status = ++*submitted == 1 ? CommandStatus::failed
                                           : CommandStatus::applied;
          value.message = "Virtual receiver rejected pairing (403)";
          completed.set_value(value);
          return completed.get_future();
        };
        if (frame.dialog == 2)
          review_dialog = ShowAddReceiverDialog(
              shell, [submitted](std::string host, unsigned port) {
                if (host != "127.0.0.1" || port != 7000)
                  throw std::runtime_error("Invalid virtual receiver input");
                ++*submitted;
                std::promise<CommandResult> completed;
                CommandResult value;
                value.status = CommandStatus::applied;
                completed.set_value(value);
                return completed.get_future();
              });
        else {
          ReceiverRecord receiver;
          receiver.receiver_id = "virtual-mac";
          receiver.name = "Virtual MacBook";
          review_dialog =
              ShowPairingDialog(shell, receiver, [result](SecureBuffer pin) {
                if (pin.Bytes().size() != 4)
                  throw std::runtime_error("Invalid virtual pairing input");
                return result();
              });
        }
        co_await winrt::resume_after(std::chrono::milliseconds(300));
        co_await ui;
        review_dialog.UpdateLayout();
        if (navigation.Page() != AppPage::playback || !review_dialog.IsLoaded())
          throw std::runtime_error(
              "Receiver input did not open on the current page");
        for (auto const& child :
             review_dialog.Content().as<StackPanel>().Children()) {
          if (auto field = child.try_as<PasswordBox>()) review_pin = field;
          if (auto field = child.try_as<TextBox>()) review_host = field;
        }
        invoke_dialog_button = [review_dialog](wchar_t const* name) {
          using namespace winrt::Microsoft::UI::Xaml;
          using namespace winrt::Microsoft::UI::Xaml::Controls;
          std::function<Button(DependencyObject)> find =
              [&](DependencyObject node) -> Button {
            if (auto button = node.try_as<Button>();
                button && button.Name() == name)
              return button;
            auto count = Media::VisualTreeHelper::GetChildrenCount(node);
            for (int index = 0; index < count; ++index)
              if (auto button =
                      find(Media::VisualTreeHelper::GetChild(node, index)))
                return button;
            return nullptr;
          };
          auto button = find(review_dialog);
          if (!button)
            throw std::runtime_error("Receiver dialog button is missing");
          Automation::Peers::ButtonAutomationPeer(button)
              .as<Automation::Provider::IInvokeProvider>()
              .Invoke();
        };
        if (review_pin && (review_dialog.IsPrimaryButtonEnabled() ||
                           review_pin.FocusState() == FocusState::Unfocused))
          throw std::runtime_error(
              "Pairing must focus an empty, protected PIN field");
        if (frame.dialog == 3) {
          review_pin.Password(L"1234");
          co_await winrt::resume_after(std::chrono::milliseconds(100));
          co_await ui;
          invoke_dialog_button(L"PrimaryButton");
        } else if (frame.dialog == 2) {
          invoke_dialog_button(L"PrimaryButton");
        }
        co_await winrt::resume_after(std::chrono::milliseconds(300));
        co_await ui;
        if (frame.dialog == 3) {
          if (*submitted != 1 || !review_pin.Password().empty() ||
              !review_pin.IsEnabled() || !FailureDialogBusy())
            throw std::runtime_error(
                "Pairing failure did not retain a retryable dialog");
          bool error_visible = false;
          for (auto const& child :
               review_dialog.Content().as<StackPanel>().Children()) {
            if (child.try_as<Expander>())
              throw std::runtime_error(
                  "Receiver errors must not require expansion");
            if (auto text = child.try_as<TextBlock>();
                text && text.Text() == LocalizedResources{}.GetString(
                                           L"PairingDialogFailed"))
              error_visible = text.Visibility() == Visibility::Visible &&
                              !text.Text().empty();
          }
          if (!error_visible)
            throw std::runtime_error(
                "Pairing failure must offer a concise retry message");
        } else if (*submitted)
          throw std::runtime_error(
              "Receiver input submitted without valid user input");
        capture = review_dialog;
      }
      co_await winrt::resume_after(std::chrono::milliseconds(750));
      co_await ui;
      using namespace winrt::Windows::Data::Json;
      auto bounds =
          [this](winrt::Microsoft::UI::Xaml::FrameworkElement const& element) {
            auto point =
                element.TransformToVisual(shell).TransformPoint({0, 0});
            JsonObject value;
            value.Insert(L"x", JsonValue::CreateNumberValue(point.X));
            value.Insert(L"y", JsonValue::CreateNumberValue(point.Y));
            value.Insert(L"width",
                         JsonValue::CreateNumberValue(element.ActualWidth()));
            value.Insert(L"height",
                         JsonValue::CreateNumberValue(element.ActualHeight()));
            return value;
          };
      JsonObject frame_report;
      frame_report.Insert(L"name", JsonValue::CreateStringValue(frame.name));
      frame_report.Insert(L"scale", JsonValue::CreateNumberValue(
                                        shell.XamlRoot().RasterizationScale()));
      if (frame.section != -1) {
        auto root = frame.page == AppPage::statistics ? statistics_page
                    : frame.page == AppPage::playback ? playback->Content()
                                                      : settings_page;
        auto title = root.FindName(frame.page == AppPage::statistics
                                       ? L"StatisticsReceiver"
                                   : frame.page == AppPage::playback
                                       ? L"CurrentReceiverCard"
                                       : L"SettingsCategory")
                         .as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
        frame_report.Insert(L"root", bounds(root));
        frame_report.Insert(L"title", bounds(title));
      }
      if (scroller)
        frame_report.Insert(
            L"scroll", JsonValue::CreateNumberValue(scroller.VerticalOffset()));
      frame_reports.Append(frame_report);
      if (frame.page == AppPage::statistics) {
        using namespace winrt::Microsoft::UI::Xaml;
        auto card = [&](wchar_t const* name) {
          return statistics_page.FindName(name).as<FrameworkElement>();
        };
        for (auto pair : {std::pair{L"NetworkHistoryCard", L"NetworkCard"},
                          std::pair{L"AudioHistoryCard", L"MetricDetails"}}) {
          auto left = card(pair.first), right = card(pair.second);
          frame_report.Insert(winrt::hstring(pair.first), bounds(left));
          frame_report.Insert(winrt::hstring(pair.second), bounds(right));
        }
      }
      winrt::Microsoft::UI::Xaml::Media::Imaging::RenderTargetBitmap bitmap;
      co_await bitmap.RenderAsync(capture);
      auto pixels = co_await bitmap.GetPixelsAsync();
      if (!bitmap.PixelWidth() || !bitmap.PixelHeight() || !pixels.Length())
        throw std::runtime_error("XAML review capture contains no pixels");
      auto file = co_await folder.CreateFileAsync(
          std::wstring(frame.name) + L".png",
          winrt::Windows::Storage::CreationCollisionOption::ReplaceExisting);
      auto output = co_await file.OpenAsync(
          winrt::Windows::Storage::FileAccessMode::ReadWrite);
      auto encoder = co_await winrt::Windows::Graphics::Imaging::BitmapEncoder::
          CreateAsync(
              winrt::Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(),
              output);
      auto reader =
          winrt::Windows::Storage::Streams::DataReader::FromBuffer(pixels);
      winrt::com_array<uint8_t> bytes(pixels.Length());
      reader.ReadBytes(bytes);
      encoder.SetPixelData(
          winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
          winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied,
          bitmap.PixelWidth(), bitmap.PixelHeight(), 96, 96, bytes);
      co_await encoder.FlushAsync();
      if (review_dialog) {
        if (frame.dialog == 1)
          invoke_dialog_button(L"SecondaryButton");
        else {
          if (review_pin) {
            review_pin.Password(L"5678");
            co_await winrt::resume_after(std::chrono::milliseconds(100));
            co_await ui;
          }
          if (review_host) review_host.Text(L"127.0.0.1");
          invoke_dialog_button(L"PrimaryButton");
        }
        auto close_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (FailureDialogBusy() &&
               std::chrono::steady_clock::now() < close_deadline) {
          co_await winrt::resume_after(std::chrono::milliseconds(50));
          co_await ui;
        }
        if (FailureDialogBusy() || navigation.Page() != AppPage::playback ||
            *submitted != (frame.dialog == 1   ? 0u
                           : frame.dialog == 2 ? 1u
                                               : 2u))
          throw std::runtime_error(
              "Receiver dialog did not cancel or finish in place: " +
              winrt::to_string(frame.name) +
              "; submitted=" + std::to_string(*submitted) +
              "; busy=" + std::to_string(FailureDialogBusy()));
      }
    }
    HideFlyout();
    Navigate(AppPage::playback);
    co_await winrt::resume_after(std::chrono::milliseconds(250));
    co_await ui;
    auto result = winrt::Windows::Data::Json::JsonObject::Parse(
        winrt::to_hstring(playback->LayoutState()));
    result.Insert(L"frames", frame_reports);
    result.Insert(
        L"dropdown_selections_verified",
        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(true));
    result.Insert(
        L"receiver_dialogs_verified",
        winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(true));
    StorageIO::AtomicText(report, winrt::to_string(result.Stringify()));
  } catch (...) {
    validation_exit = E_FAIL;
    StorageIO::AtomicText(report, DescribeFailure().message);
  }
  ui_report.clear();
  refresh.Start();
  RequestClose();
}
}  // namespace Win2Air
