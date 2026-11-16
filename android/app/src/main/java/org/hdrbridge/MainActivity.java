package org.hdrbridge;

import android.app.*;
import android.content.*;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.*;
import android.provider.OpenableColumns;
import android.view.*;
import android.widget.*;
import java.util.*;

public class MainActivity extends Activity {
  static final int PICK = 1, TREE = 2, NOTIFICATIONS = 3;
  int pendingStart = -2;
  AppModel model;
  android.content.SharedPreferences prefs;
  Uri folder;
  Options current;
  AppModel.Source activeSource;
  LinearLayout sourceList, taskList;
  TextView details, summary, folderLabel;
  LinearLayout filesPage, tasksPage, optionsPanel, optionsFooter;
  Button filesTab, tasksTab, startButton;
  ScrollView filesScroll, tasksScroll;
  boolean tasksSelected;
  final Map<Uri, String> outputNames = new WeakHashMap<>();
  Handler handler = new Handler(Looper.getMainLooper());
  UpdateManager updates;
  TextView versionView;
  String fullVersion = "", shortVersion = "";
  AlertDialog updateDialog;
  boolean resumed;

  @Override
  public void onCreate(Bundle state) {
    super.onCreate(state);
    if (state != null) {
      pendingStart = state.getInt("pendingStart", -2);
      tasksSelected = state.getBoolean("tasksSelected", false);
    }
    model = AppModel.get(this);
    updates = UpdateManager.get(this);
    prefs = getSharedPreferences("android_options_v2", 0);
    current = Options.load(prefs, prefs.getInt("format", 0));
    current.useSuffix = prefs.getBoolean("useSuffixGlobal", current.useSuffix);
    String f = prefs.getString("folder", "");
    if (!f.isEmpty())
      folder = Uri.parse(f);
    build();
  }
  @Override protected void onSaveInstanceState(Bundle state) {
    state.putInt("pendingStart", pendingStart);
    state.putBoolean("tasksSelected", tasksSelected);
    super.onSaveInstanceState(state);
  }
  @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] grants) {
    super.onRequestPermissionsResult(request, permissions, grants);
    if (request != NOTIFICATIONS || pendingStart == -2) return;
    int next = pendingStart;
    pendingStart = -2;
    if (next == -1) startQueue(null);
    else synchronized (model) { if (next >= 0 && next < model.tasks.size()) startQueue(model.tasks.get(next)); }
  }
  @Override
  protected void onResume() {
    super.onResume();
    resumed = true;
    model.listener = () -> handler.post(this::refresh);
    updates.setListener(() -> handler.post(() -> { if (resumed) refreshUpdate(); }));
    if (updates.installPending()) updates.resumeInstall(this);
    if (updates.state == UpdateManager.State.IDLE || updates.state == UpdateManager.State.UP_TO_DATE
        || updates.state == UpdateManager.State.UNAVAILABLE || updates.state == UpdateManager.State.ERROR)
      updates.maybeCheck();
    refreshUpdate();
    refresh();
  }
  @Override
  protected void onPause() {
    resumed = false;
    updates.setListener(null);
    if (updateDialog != null) { updateDialog.dismiss(); updateDialog = null; }
    model.listener = null;
    super.onPause();
  }
  void openUpdates() {
    if (updates.state == UpdateManager.State.IDLE || updates.state == UpdateManager.State.UP_TO_DATE)
      updates.check(true);
    updateDialog = new AlertDialog.Builder(this).setTitle("App updates")
        .setMessage("").setPositiveButton("Close", null).setNegativeButton("Close", null).create();
    updateDialog.setOnDismissListener(d -> updateDialog = null);
    updateDialog.show();
    refreshUpdate();
  }
  void refreshUpdate() {
    if (resumed) updates.launchConfirmation(this);
    UpdateManager.State state = updates.state;
    boolean available = state == UpdateManager.State.AVAILABLE || state == UpdateManager.State.READY;
    if (versionView != null) {
      versionView.setText(shortVersion + (available ? " •" : ""));
      versionView.setContentDescription(fullVersion + (available ? ", update available" : ", check for updates"));
    }
    if (updateDialog == null || !updateDialog.isShowing()) return;
    String message = updates.message;
    if (state == UpdateManager.State.AVAILABLE && updates.release != null)
      message = "Version " + updates.release.version + " is available.";
    if (state == UpdateManager.State.DOWNLOADING) message = "Downloading update: " + updates.progress + "%";
    updateDialog.setMessage(message.isEmpty() ? "Check for updates" : message);
    Button action = updateDialog.getButton(AlertDialog.BUTTON_POSITIVE);
    Button close = updateDialog.getButton(AlertDialog.BUTTON_NEGATIVE);
    close.setVisibility(View.VISIBLE);
    action.setVisibility(View.VISIBLE);
    if (state == UpdateManager.State.AVAILABLE) {
      action.setText("Download");
      action.setOnClickListener(v -> updates.download());
    } else if (state == UpdateManager.State.READY) {
      action.setText("Install");
      action.setOnClickListener(v -> {
        synchronized (model) {
          if (model.running) { updateDialog.setMessage("Wait for conversion to finish before installing."); return; }
          for (AppModel.Source source : model.sources) if (source.inspecting) {
            updateDialog.setMessage("Wait for inspection to finish before installing."); return;
          }
        }
        updates.install(this);
      });
    } else if (state == UpdateManager.State.INSTALLING && updates.canCancelInstall()) {
      action.setText("Cancel installation");
      action.setOnClickListener(v -> updates.cancelInstall());
    } else if (state == UpdateManager.State.ERROR || state == UpdateManager.State.UNAVAILABLE) {
      action.setText("Retry");
      action.setOnClickListener(v -> updates.check(true));
    } else {
      action.setVisibility(View.GONE);
    }
  }
  int dp(int n) {
    return Math.round(n * getResources().getDisplayMetrics().density);
  }
  void build() {
    LinearLayout root = new LinearLayout(this);
    root.setOrientation(LinearLayout.VERTICAL);
    root.setOnApplyWindowInsetsListener((v, i) -> {
      v.setPadding(dp(12) + i.getSystemWindowInsetLeft(), dp(8) + i.getSystemWindowInsetTop(),
          dp(12) + i.getSystemWindowInsetRight(), dp(8) + i.getSystemWindowInsetBottom());
      return i;
    });
    setContentView(root);
    TextView title = new TextView(this);
    title.setText("HDR Bridge");
    title.setTextSize(20);
    title.setTypeface(android.graphics.Typeface.create("sans-serif-medium", android.graphics.Typeface.NORMAL));
    LinearLayout header = row(root);
    header.setGravity(Gravity.CENTER_VERTICAL);
    LinearLayout identity = new LinearLayout(this);
    identity.setOrientation(LinearLayout.HORIZONTAL);
    identity.setBaselineAligned(true);
    identity.setGravity(Gravity.START | Gravity.BOTTOM);
    identity.addView(title);
    TextView version = new TextView(this);
    versionView = version;
    version.setTag("app_version");
    version.setTextSize(12);
    version.setTranslationY(-dp(1));
    version.setIncludeFontPadding(false);
    version.setSingleLine(true);
    version.setEllipsize(android.text.TextUtils.TruncateAt.END);
    version.setTextColor(themeColor(android.R.attr.textColorSecondary));
    try {
      fullVersion = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
      shortVersion = fullVersion.split("-", 2)[0];
      version.setText(shortVersion);
      version.setContentDescription(fullVersion + ", check for updates");
      version.setOnClickListener(v -> openUpdates());
    } catch (android.content.pm.PackageManager.NameNotFoundException ignored) {
      version.setVisibility(View.GONE);
    }
    LinearLayout.LayoutParams versionLayout = new LinearLayout.LayoutParams(0, -2, 1);
    versionLayout.setMarginStart(dp(8));
    identity.addView(version, versionLayout);
    header.addView(identity, new LinearLayout.LayoutParams(0, -2, 1));
    Button reset = new Button(this);
    reset.setText("Reset settings");
    reset.setTextSize(14);
    reset.setAllCaps(false);
    reset.setTag("reset_settings");
    reset.setPadding(dp(8), 0, dp(8), 0);
    header.addView(reset, new LinearLayout.LayoutParams(-2, dp(36)));
    reset.setOnClickListener(v -> confirmResetSettings());
    FrameLayout pages = new FrameLayout(this);
    root.addView(pages, new LinearLayout.LayoutParams(-1, 0, 1));
    filesPage = new LinearLayout(this);
    filesPage.setOrientation(LinearLayout.VERTICAL);
    pages.addView(filesPage);
    tasksPage = new LinearLayout(this);
    tasksPage.setOrientation(LinearLayout.VERTICAL);
    pages.addView(tasksPage);
    heading(filesPage, "OUTPUT");
    LinearLayout outputArea = new LinearLayout(this);
    outputArea.setTag("output_region");
    outputArea.setOrientation(LinearLayout.VERTICAL);
    filesPage.addView(outputArea, new LinearLayout.LayoutParams(-1, -2));
    folderLabel = new TextView(this);
    folderLabel.setTextSize(14);
    folderLabel.setSingleLine(true);
    folderLabel.setEllipsize(android.text.TextUtils.TruncateAt.END);
    folderLabel.setTextColor(accentColor());
    folderLabel.setGravity(Gravity.CENTER_VERTICAL);
    folderLabel.setCompoundDrawablesRelativeWithIntrinsicBounds(android.R.drawable.ic_menu_save, 0, 0, 0);
    folderLabel.setCompoundDrawablePadding(dp(6));
    folderLabel.setOnClickListener(v -> pickFolder());
    outputArea.addView(folderLabel, new LinearLayout.LayoutParams(-1, dp(32)));
    ScrollView outputScroll = new ScrollView(this) {
      @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int max = (int) (getResources().getDisplayMetrics().heightPixels * 0.55f);
        int offered = View.MeasureSpec.getSize(heightSpec);
        int mode = View.MeasureSpec.getMode(heightSpec);
        if (mode != View.MeasureSpec.UNSPECIFIED) max = Math.min(max, offered);
        super.onMeasure(widthSpec, View.MeasureSpec.makeMeasureSpec(max, View.MeasureSpec.AT_MOST));
      }
    };
    outputArea.addView(outputScroll, new LinearLayout.LayoutParams(-1, -2));
    LinearLayout outputContents = new LinearLayout(this);
    outputContents.setOrientation(LinearLayout.VERTICAL);
    outputScroll.addView(outputContents);
    optionsPanel = new LinearLayout(this);
    optionsPanel.setOrientation(LinearLayout.VERTICAL);
    outputContents.addView(optionsPanel);
    optionsFooter = new LinearLayout(this);
    optionsFooter.setOrientation(LinearLayout.VERTICAL);
    outputContents.addView(optionsFooter);
    heading(filesPage, "FILES");
    LinearLayout r = row(filesPage);
    button(r, "Add files", this::pick);
    r.getChildAt(0).setTag("add_files");
    button(r, "Remove", () -> { model.removeSelectedSources(); refresh(); });
    button(r, "Clear", () -> { model.clearSources(); refresh(); });
    filesScroll = new ScrollView(this);
    styleListPanel(filesScroll);
    filesPage.addView(filesScroll, listLayout());
    sourceList = new LinearLayout(this);
    sourceList.setOrientation(LinearLayout.VERTICAL);
    filesScroll.addView(sourceList);
    r = row(filesPage);
    bottomBarSpacing(r);
    button(r, "Add to queue", this::addTasks);
    r.getChildAt(0).setTag("add_to_queue");
    heading(tasksPage, "TASKS");
    summary = new TextView(this);
    tasksPage.addView(summary);
    tasksScroll = new ScrollView(this);
    styleListPanel(tasksScroll);
    tasksPage.addView(tasksScroll, listLayout());
    taskList = new LinearLayout(this);
    taskList.setOrientation(LinearLayout.VERTICAL);
    tasksScroll.addView(taskList);
    r = row(tasksPage);
    bottomBarSpacing(r);
    button(r, "Remove all", () -> {
      if (model.running) {
        toast("Stop the queue before removing tasks");
        return;
      }
      model.removeAllTasks();
      refresh();
    });
    r.getChildAt(0).setTag("remove_all");
    startButton = new Button(this);
    startButton.setAllCaps(false);
    startButton.setTextSize(14);
    startButton.setTag("start_all");
    startButton.setMinHeight(dp(36));
    startButton.setPadding(dp(4), 0, dp(4), 0);
    r.addView(startButton, buttonLayout());
    startButton.setOnClickListener(v -> {
      if (model.running) model.cancel();
      else startQueue(null);
    });
    r = row(root);
    filesTab = new Button(this);
    filesTab.setAllCaps(false);
    filesTab.setText("FILES");
    filesTab.setTag("files_tab");
    tasksTab = new Button(this);
    tasksTab.setAllCaps(false);
    tasksTab.setText("TASKS");
    tasksTab.setTag("tasks_tab");
    r.addView(filesTab, sizedButtonLayout(40));
    r.addView(tasksTab, sizedButtonLayout(40));
    filesTab.setOnClickListener(v -> showPage(false));
    tasksTab.setOnClickListener(v -> showPage(true));
    renderInline();
    showPage(tasksSelected);
  }
  void showPage(boolean tasks) {
    tasksSelected = tasks;
    filesPage.setVisibility(tasks ? View.GONE : View.VISIBLE);
    tasksPage.setVisibility(tasks ? View.VISIBLE : View.GONE);
    filesTab.setSelected(!tasks);
    tasksTab.setSelected(tasks);
    filesTab.setBackgroundColor(tasks ? Color.TRANSPARENT : getColor(R.color.tab_selected));
    tasksTab.setBackgroundColor(tasks ? getColor(R.color.tab_selected) : Color.TRANSPARENT);
    filesTab.setTextColor(tasks ? secondaryTextColor() : accentColor());
    tasksTab.setTextColor(tasks ? accentColor() : secondaryTextColor());
  }
  int themeColor(int attr) {
    android.content.res.TypedArray attrs = obtainStyledAttributes(new int[]{attr});
    int color = attrs.getColor(0, Color.GRAY);
    attrs.recycle();
    return color;
  }
  int accentColor() { return themeColor(android.R.attr.colorAccent); }
  int secondaryTextColor() {
    android.content.res.TypedArray attrs = obtainStyledAttributes(new int[]{android.R.attr.textColorPrimary});
    int color = attrs.getColor(0, Color.GRAY);
    attrs.recycle();
    return color;
  }
  String folderName(Uri uri) {
    try {
      String id = android.provider.DocumentsContract.getTreeDocumentId(uri);
      int colon = id.indexOf(':');
      return colon >= 0 && colon + 1 < id.length() ? id.substring(colon + 1) : id;
    } catch (Exception ignored) {
      String name = uri.getLastPathSegment();
      return name == null ? "Output folder" : name;
    }
  }
  final class ChoiceAdapter extends ArrayAdapter<String> {
    ChoiceAdapter(String[] values) {
      super(MainActivity.this, android.R.layout.simple_spinner_dropdown_item, values);
    }
    @Override public View getView(int position, View convertView, android.view.ViewGroup parent) {
      TextView text = (TextView) super.getView(position, convertView, parent);
      text.setTextSize(15);
      text.setSingleLine(true);
      text.setEllipsize(android.text.TextUtils.TruncateAt.END);
      text.setPadding(dp(8), 0, dp(8), 0);
      text.setGravity(Gravity.CENTER_VERTICAL);
      return text;
    }
    @Override public View getDropDownView(int position, View convertView, android.view.ViewGroup parent) {
      TextView text = (TextView) super.getDropDownView(position, convertView, parent);
      text.setTextSize(16);
      text.setMinHeight(dp(48));
      text.setGravity(Gravity.CENTER_VERTICAL);
      return text;
    }
  }
  void saveCurrent() {
    try { prefs.edit().putInt("format", current.format).putBoolean("useSuffixGlobal", current.useSuffix)
      .putString("options_" + current.format, current.save().toString()).apply(); }
    catch (Exception e) { toast(AppModel.message(e)); }
  }
  void confirmResetSettings() {
    new AlertDialog.Builder(this)
        .setTitle("Reset output settings?")
        .setMessage("Restore defaults for all output formats.")
        .setNegativeButton("Cancel", null)
        .setPositiveButton("Reset", (dialog, which) -> resetOutputSettings()).show();
  }
  void resetOutputSettings() {
    int format = current.format;
    android.content.SharedPreferences.Editor edit = prefs.edit();
    for (int i = 0; i < Options.LABELS.length; i++) edit.remove("options_" + i);
    edit.remove("format").remove("useSuffixGlobal").apply();
    current = Options.defaults(format);
    saveCurrent();
    renderInline();
  }
  void inlineChoice(LinearLayout parent, boolean cell, String label, String[] values, int index,
      String tag, java.util.function.IntConsumer onChange) {
    LinearLayout container = new LinearLayout(this);
    container.setOrientation(cell ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
    container.setGravity(Gravity.CENTER_VERTICAL);
    parent.addView(container, new LinearLayout.LayoutParams(cell ? 0 : -1, -2, cell ? 1 : 0));
    TextView text = new TextView(this);
    text.setText(label);
    text.setTextSize(13);
    text.setSingleLine(true);
    container.addView(text, cell ? new LinearLayout.LayoutParams(-1, -2)
        : new LinearLayout.LayoutParams(0, -2, 1));
    Spinner control = new Spinner(this);
    control.setTag(tag);
    control.setMinimumHeight(dp(36));
    control.setAdapter(new ChoiceAdapter(values));
    control.setSelection(index);
    container.addView(control, cell ? new LinearLayout.LayoutParams(-1, dp(36))
        : new LinearLayout.LayoutParams(0, dp(36), 2));
    control.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
      public void onNothingSelected(AdapterView<?> p) {}
      public void onItemSelected(AdapterView<?> p, View v, int n, long id) {
        if (n != index) {
          onChange.accept(n);
          saveCurrent();
          renderInline();
        }
      }
    });
  }
  void renderInline() {
    optionsPanel.removeAllViews();
    optionsFooter.removeAllViews();
    inlineChoice(optionsPanel, false, "Format", Options.LABELS, current.format, "output_format", n -> {
      saveCurrent();
      boolean suffix = current.useSuffix;
      current = Options.load(prefs, n);
      current.useSuffix = suffix;
    });
    int f = current.format;
    boolean gamut = f == 0 || f == 1 || f == 2 || f == 3 || f == 5 || f == 7;
    boolean transfer = f == 1 || f == 3 || f == 5;
    if (gamut) {
      LinearLayout pair = row(optionsPanel);
      String[] values = (f == 0 || f == 7) ? new String[]{"Rec.709", "Display P3"}
          : current.gainmap() ? new String[]{"Rec.709", "Display P3", "Rec.2020"}
          : new String[]{"Rec.2020", "Display P3"};
      String[] codes = (f == 0 || f == 7) ? new String[]{"rec709", "p3"}
          : current.gainmap() ? new String[]{"rec709", "p3", "rec2020"}
          : new String[]{"rec2020", "p3"};
      inlineChoice(pair, true, "Color gamut", values,
          Math.max(0, Arrays.asList(codes).indexOf(current.gamut)), "output_gamut",
          n -> current.gamut = codes[n]);
      if (transfer) {
        String[] choices = f == 1 ? new String[]{"PQ", "HLG"}
            : new String[]{"PQ", "HLG", "Gain map"};
        inlineChoice(pair, true, "HDR representation", choices, current.transfer,
            "output_transfer", n -> {
              current.transfer = n;
              current.gamut = current.gainmap() ? "p3" : "rec2020";
              current.suffix = Options.defaultSuffix(current.format, n);
            });
      } else if (f == 0 || f == 7) {
        inlineChoice(pair, true, "Target peak", new String[]{"Faithful / Auto", "1000 nits",
            "2000 nits", "4000 nits", "10000 nits"}, current.peak, "output_peak",
            n -> current.peak = n);
      } else if (f == 2) {
        inlineChoice(pair, true, "TIFF compression", new String[]{"Compressed", "Uncompressed"},
            current.compressed ? 0 : 1, "output_compression", n -> current.compressed = n == 0);
      }
    }
    if (current.gainmap()) {
      LinearLayout pair = row(optionsPanel);
      inlineChoice(pair, true, "Gain map size", new String[]{"Quarter", "Half", "Full"},
          current.scale == 4 ? 0 : current.scale == 1 ? 2 : 1, "output_scale",
          n -> current.scale = new int[]{4, 2, 1}[n]);
      if (f == 0 || f == 7)
        inlineChoice(pair, true, "Channels", new String[]{"Mono", "RGB"}, current.channels,
            "output_channels", n -> current.channels = n);
      else
        inlineChoice(pair, true, "Target peak", new String[]{"Faithful / Auto", "1000 nits",
            "2000 nits", "4000 nits", "10000 nits"}, current.peak, "output_peak",
            n -> current.peak = n);
    }
    if (f == 6)
      inlineChoice(optionsPanel, false, "Export", new String[]{"Original", "PNG", "TIFF", "JPEG"},
          current.extraction, "output_extraction", n -> current.extraction = n);
    LinearLayout checks = row(optionsFooter);
    boolean forcedLossless = f == 1 || f == 2;
    CheckBox lossless = inlineCheck(checks, "Lossless", f != 7 && (forcedLossless || current.lossless),
        "output_lossless");
    lossless.setEnabled(f == 3 || f == 4);
    lossless.setOnCheckedChangeListener((v, checked) -> {
      current.lossless = checked;
      saveCurrent();
      renderInline();
    });
    CheckBox exif = inlineCheck(checks, "EXIF", f != 7 && current.exif, "output_exif");
    exif.setEnabled(f != 7);
    exif.setOnCheckedChangeListener((v, checked) -> { current.exif = checked; saveCurrent(); });
    CheckBox xmp = inlineCheck(checks, "XMP", f != 7 && current.xmp, "output_xmp");
    xmp.setEnabled(f != 7);
    xmp.setOnCheckedChangeListener((v, checked) -> { current.xmp = checked; saveCurrent(); });
    if (f != 6 || current.extraction == 3) {
      LinearLayout qualityRow = row(optionsFooter);
      qualityRow.setGravity(Gravity.CENTER_VERTICAL);
      TextView quality = new TextView(this);
      quality.setTextSize(14);
      String label = f == 1 || f == 2 ? "Compression: " : "Quality: ";
      quality.setText(label + current.quality);
      qualityRow.addView(quality);
      SeekBar seek = new SeekBar(this);
      seek.setTag("output_quality");
      seek.setMin(1);
      seek.setMax(f == 1 || f == 2 ? 9 : 100);
      seek.setProgress(current.quality);
      seek.setEnabled(!((f == 3 || f == 4) && current.lossless)
          && !(f == 2 && !current.compressed));
      qualityRow.addView(seek, new LinearLayout.LayoutParams(0, dp(36), 1));
      seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
        public void onStartTrackingTouch(SeekBar b) {}
        public void onStopTrackingTouch(SeekBar b) { saveCurrent(); }
        public void onProgressChanged(SeekBar b, int n, boolean user) {
          if (user) { current.quality = n; quality.setText(label + n); }
        }
      });
    }
    LinearLayout suffixRow = row(optionsFooter);
    suffixRow.setGravity(Gravity.CENTER_VERTICAL);
    CheckBox suffixCheck = new CheckBox(this);
    suffixCheck.setText("Use suffix");
    suffixCheck.setTextSize(14);
    suffixCheck.setMinHeight(0);
    suffixCheck.setChecked(current.useSuffix);
    suffixCheck.setTag("output_use_suffix");
    suffixRow.addView(suffixCheck, new LinearLayout.LayoutParams(-2, dp(32)));
    EditText suffix = new EditText(this);
    suffix.setTag("output_suffix");
    suffix.setSingleLine(true);
    suffix.setTextSize(14);
    suffix.setPadding(dp(6), 0, dp(6), 0);
    suffix.setText(current.suffix);
    suffix.setEnabled(current.useSuffix);
    suffixRow.addView(suffix, new LinearLayout.LayoutParams(0, dp(32), 1));
    suffixCheck.setOnCheckedChangeListener((v, checked) -> {
      current.useSuffix = checked;
      suffix.setEnabled(checked);
      saveCurrent();
    });
    suffix.addTextChangedListener(new android.text.TextWatcher() {
      public void beforeTextChanged(CharSequence text, int start, int count, int after) {}
      public void onTextChanged(CharSequence text, int start, int before, int count) {
        current.suffix = text.toString();
        saveCurrent();
      }
      public void afterTextChanged(android.text.Editable text) {}
    });
  }
  CheckBox inlineCheck(LinearLayout parent, String label, boolean checked, String tag) {
    CheckBox check = new CheckBox(this);
    check.setText(label);
    check.setTextSize(14);
    check.setMinHeight(dp(36));
    check.setTag(tag);
    check.setChecked(checked);
    parent.addView(check, new LinearLayout.LayoutParams(0, dp(36), 1));
    return check;
  }

  LinearLayout row(LinearLayout p) {
    LinearLayout r = new LinearLayout(this);
    p.addView(r);
    return r;
  }
  void button(LinearLayout p, String text, Runnable action) {
    Button b = new Button(this);
    b.setText(text);
    b.setAllCaps(false);
    b.setTextSize(14);
    b.setMinHeight(dp(36));
    b.setPadding(dp(4), 0, dp(4), 0);
    p.addView(b, buttonLayout());
    b.setOnClickListener(v -> action.run());
  }
  LinearLayout.LayoutParams buttonLayout() {
    return sizedButtonLayout(36);
  }
  LinearLayout.LayoutParams sizedButtonLayout(int height) {
    LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, dp(height), 1);
    params.setMargins(dp(2), 0, dp(2), 0);
    return params;
  }
  LinearLayout.LayoutParams listLayout() {
    LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, 0, 1);
    params.setMargins(0, dp(6), 0, dp(8));
    return params;
  }
  void bottomBarSpacing(LinearLayout bar) {
    LinearLayout.LayoutParams params = (LinearLayout.LayoutParams) bar.getLayoutParams();
    params.bottomMargin = dp(6);
    bar.setLayoutParams(params);
  }
  void styleListPanel(ScrollView scroll) {
    scroll.setBackgroundResource(R.drawable.list_panel);
    scroll.setPadding(dp(6), dp(6), dp(6), dp(6));
    scroll.setClipToPadding(true);
  }
  void heading(LinearLayout p, String text) {
    TextView t = new TextView(this);
    t.setText(text);
    t.setTextSize(18);
    t.setPadding(0, dp(6), 0, dp(4));
    p.addView(t);
  }
  void pick() {
    Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                   .setType("*/*")
                   .addCategory(Intent.CATEGORY_OPENABLE)
                   .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                   .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                       | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
    startActivityForResult(i, PICK);
  }
  void pickFolder() {
    Intent i =
        new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
    startActivityForResult(i, TREE);
  }
  @Override
  protected void onActivityResult(int request, int result, Intent data) {
    super.onActivityResult(request, result, data);
    if (result != RESULT_OK || data == null)
      return;
    if (request == TREE) {
      Uri chosenFolder = data.getData();
      try {
        persistTree(chosenFolder, data.getFlags());
        folder = chosenFolder;
        prefs.edit().putString("folder", folder.toString()).apply();
      } catch (Exception e) {
        toast(AppModel.message(e));
      }
    } else if (request == PICK) {
      ArrayList<Uri> uris = new ArrayList<>();
      if (data.getClipData() != null) {
        for (int i = 0; i < data.getClipData().getItemCount(); i++)
          uris.add(data.getClipData().getItemAt(i).getUri());
      } else if (data.getData() != null)
        uris.add(data.getData());
      for (Uri u : uris) {
        try {
          getContentResolver().takePersistableUriPermission(
              u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
        }
        AppModel.Source s = model.add(u, displayName(u));
        model.inspect(s);
      }
    }
    refresh();
  }
  void persistTree(Uri u, int flags) {
    boolean read = (flags & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0,
            write = (flags & Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0;
    if (read && write)
      getContentResolver().takePersistableUriPermission(
          u, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
    else if (read)
      getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION);
    else if (write)
      getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
    else
      throw new SecurityException("Folder permission was not granted");
    if (!read || !write)
      throw new SecurityException("Folder needs read and write access");
  }
  String displayName(Uri u) {
    try (Cursor c = getContentResolver().query(
             u, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
      if (c != null && c.moveToFirst())
        return safeName(c.getString(0));
    } catch (Exception ignored) {
    }
    return "image";
  }
  String outputName(Uri uri, String fallback) {
    String cached = outputNames.get(uri);
    if (cached != null) return cached;
    String name = fallback;
    try (Cursor c = getContentResolver().query(
             uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
      if (c != null && c.moveToFirst() && c.getString(0) != null) {
        name = c.getString(0);
        outputNames.put(uri, name);
        return name;
      }
    } catch (Exception ignored) {
    }
    try {
      String id = android.provider.DocumentsContract.getDocumentId(uri);
      int slash = id.lastIndexOf('/');
      name = slash >= 0 ? id.substring(slash + 1) : id;
    } catch (Exception ignored) {
    }
    outputNames.put(uri, name);
    return name;
  }
  static String safeName(String s) {
    if (s == null)
      return "image";
    s = s.replaceAll("[\\\\/:*?\"<>|\\p{Cntrl}]", "_").trim();
    return s.isEmpty() ? "image" : s;
  }
  void addTasks() {
    if (current.useSuffix && current.suffix.matches(".*[\\\\/:*?\"<>|\\p{Cntrl}].*")) {
      toast("Suffix contains an invalid character");
      return;
    }
    if (folder == null) {
      toast("Select an output folder first");
      return;
    }
    boolean any = false;
    synchronized (model) {
      for (AppModel.Source s : model.sources) any |= s.selected;
    }
    if (!any) {
      toast("Select files first");
      return;
    }
    model.addTasks(current, folder);
    refresh();
    toast("Added to queue");
    showPage(true);
  }
  void showInspector(AppModel.Source s) {
    activeSource = s; details = new TextView(this); details.setTextSize(14); details.setTextIsSelectable(true); details.setPadding(dp(20), dp(12), dp(20), dp(12));
    ScrollView scroll = new ScrollView(this); scroll.addView(details); inspectView(s);
    AlertDialog dialog = new AlertDialog.Builder(this).setTitle("SOURCE INSPECTOR").setView(scroll).setPositiveButton("Close", null).create();
    dialog.setOnDismissListener(d -> { activeSource = null; details = null; }); dialog.show();
  }
  void inspectView(AppModel.Source s) {
    if (s.info == null) {
      details.setText(s.name + "\n"
          + (s.inspecting             ? "Inspecting…"
                  : s.error.isEmpty() ? "Waiting"
                                      : "Inspection failed: " + s.error));
      if (!s.inspecting && s.error.isEmpty())
        model.inspect(s);
      return;
    }
    org.json.JSONObject j = s.info;
    StringBuilder b = new StringBuilder(s.name)
                          .append("\n")
                          .append(j.optString("format"))
                          .append(" • ")
                          .append(j.optString("assetKind"))
                          .append("\n")
                          .append(j.optInt("width"))
                          .append(" × ")
                          .append(j.optInt("height"))
                          .append(" • ")
                          .append(j.optInt("bitDepth"))
                          .append(" bit • ")
                          .append(j.optString("codec"))
                          .append(" • ")
                          .append(j.optString("pixelFormat"));
    section(b, j, "resolvedColor", "Resolved color");
    section(b, j, "nativeSignal", "Native color signal");
    section(b, j, "iccSignal", "ICC profile signal");
    section(b, j, "metadata", "Metadata status");
    if (j.optBoolean("gainMapPresent")) {
      b.append("\nGain map family: ").append(j.optString("gainMapFamily"));
      section(b, j, "baseRendition", "Base rendition");
      section(b, j, "gainMapSize", "Gain map size");
      section(b, j, "gainMapLayout", "Gain map layout");
      section(b, j, "gainMapMetadata", "Gain map metadata");
      section(b, j, "reconstructedHdr", "Reconstructed HDR");
    }
    details.setText(b.toString());
  }
  void section(StringBuilder b, org.json.JSONObject root, String key, String title) {
    org.json.JSONObject j = root.optJSONObject(key);
    if (j == null)
      return;
    b.append("\n\n").append(title).append(": ");
    Iterator<String> keys = j.keys();
    boolean first = true;
    while (keys.hasNext()) {
      String k = keys.next();
      if (!first)
        b.append(" • ");
      b.append(k).append(" ").append(j.optString(k));
      first = false;
    }
  }
  void refresh() {
    if (sourceList == null)
      return;
    folderLabel.setText(folder == null ? "Choose output folder" : "Save to: " + folderName(folder));
    int filesY = filesScroll.getScrollY();
    int tasksY = tasksScroll.getScrollY();
    sourceList.removeAllViews();
    taskList.removeAllViews();
    int ready = 0, done = 0, failed = 0, skipped = 0, cancelled = 0;
    synchronized (model) {
      for (AppModel.Source s : model.sources) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(0, dp(3), 0, dp(3));
        sourceList.addView(box);
        LinearLayout line = row(box);
        line.setGravity(Gravity.CENTER_VERTICAL);
        CheckBox c = new CheckBox(this);
        c.setChecked(s.selected);
        c.setMinHeight(0);
        c.setMinimumHeight(0);
        line.addView(c, new LinearLayout.LayoutParams(-2, dp(36)));
        c.setOnCheckedChangeListener((v, checked) -> s.selected = checked);
        TextView name = new TextView(this);
        name.setTextSize(16);
        name.setText(s.name);
        line.addView(name, new LinearLayout.LayoutParams(0, -2, 1));
        TextView meta = new TextView(this);
        meta.setTextSize(13);
        if (s.info != null) {
          org.json.JSONObject j = s.info;
          String k = j.optString("assetKind");
          String kind = k.equals("gain-map-hdr") ? "HDR gain map"
              : k.equals("direct-hdr") ? "Direct HDR" : k;
          String format = j.optString("format");
          if (format.equalsIgnoreCase("Ultra HDR JPEG") && k.equals("gain-map-hdr"))
            kind = "";
          meta.setText(format + (kind.isEmpty() ? "" : " • " + kind)
              + (j.optInt("width") > 0 ? " • " + j.optInt("width") + "×" + j.optInt("height") : "")
              + (j.optInt("bitDepth") > 0 ? " • " + j.optInt("bitDepth") + "-bit" : ""));
        } else {
          meta.setText(s.inspecting ? "Inspecting…" : s.error.isEmpty() ? "Waiting" : "Inspection failed");
        }
        if (!s.error.isEmpty() && s.info == null)
          meta.setTextColor(Color.rgb(200, 55, 55));
        box.addView(meta);
        line = row(box);
        line.setPadding(0, dp(2), 0, 0);
        button(line, "Preview", () -> preview(s.uri));
        line.getChildAt(0).setTag("source_preview");
        button(line, "Inspect", () -> showInspector(s));
        line.getChildAt(1).setTag("source_inspect");
        for (int i = 0; i < line.getChildCount(); i++) {
          Button action = (Button) line.getChildAt(i);
          action.setMinHeight(0);
          action.setPadding(0, 0, 0, 0);
          action.setLayoutParams(sizedButtonLayout(32));
        }
      }
      for (AppModel.Task t : model.tasks) {
        switch (t.status) {
          case "Ready":
            ready++;
            break;
          case "Completed":
            done++;
            break;
          case "Failed":
            failed++;
            break;
          case "Skipped":
            skipped++;
            break;
          case "Cancelled":
            cancelled++;
            break;
        }
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(8), dp(12), dp(8), dp(12));
        android.graphics.drawable.GradientDrawable border = new android.graphics.drawable.GradientDrawable();
        border.setColor(Color.TRANSPARENT);
        border.setStroke(dp(1), themeColor(android.R.attr.colorControlNormal));
        border.setCornerRadius(dp(6));
        box.setBackground(border);
        LinearLayout.LayoutParams card = new LinearLayout.LayoutParams(-1, -2);
        card.setMargins(0, dp(4), 0, dp(4));
        taskList.addView(box, card);
        TextView first = new TextView(this);
        String sourceFormat = t.source.info == null ? "Image" : t.source.info.optString("format", "Image");
        if (sourceFormat.isEmpty()) sourceFormat = "Image";
        first.setText(sourceFormat + " → " + Options.LABELS[t.options.format] + " • " + t.status);
        first.setTextSize(16);
        if (t.status.equals("Failed") || t.status.equals("Cancelled") || t.status.equals("Skipped"))
          first.setTextColor(Color.rgb(220, 60, 60));
        else if (t.status.equals("Completed"))
          first.setTextColor(Color.rgb(45, 160, 80));
        else if (t.status.equals("Running"))
          first.setTextColor(Color.rgb(40, 130, 230));
        box.addView(first);
        TextView second = new TextView(this);
        String destination = t.folder.getLastPathSegment();
        String outputName = t.output == null ? t.filename : outputName(t.output, t.filename);
        second.setText(t.source.name + " → " + outputName + "\n"
            + (destination == null ? "Output folder" : destination) + " • " + t.detail);
        second.setTextSize(14);
        box.addView(second);
        ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);
        bar.setProgress(t.progress);
        box.addView(bar);
        box.setOnClickListener(v -> taskMenu(t));
      }
    }
    filesScroll.post(() -> filesScroll.scrollTo(0, filesY));
    tasksScroll.post(() -> tasksScroll.scrollTo(0, tasksY));
    if (activeSource != null && details != null) inspectView(activeSource);
    startButton.setText(model.running ? "Stop" : "Start all");
    tasksTab.setText(model.tasks.isEmpty() ? "TASKS" : "TASKS (" + model.tasks.size() + ")");
    summary.setText("Ready " + ready + " • Completed " + done + " • Skipped " + skipped
        + " • Failed " + failed + " • Cancelled " + cancelled
        + (model.running ? " • Running" : ""));
  }
  void startQueue(AppModel.Task t) {
    if (Build.VERSION.SDK_INT >= 33 &&
        checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != android.content.pm.PackageManager.PERMISSION_GRANTED &&
        !prefs.getBoolean("notificationsAsked", false)) {
      pendingStart = t == null ? -1 : model.tasks.indexOf(t);
      prefs.edit().putBoolean("notificationsAsked", true).apply();
      requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS}, NOTIFICATIONS);
      return;
    }
    try {
      model.start(t);
    } catch (Exception e) {
      toast("Unable to start conversion: " + AppModel.message(e));
    }
  }
  void taskMenu(AppModel.Task t) {
    if (model.running) {
      toast("Stop the queue before editing tasks");
      return;
    }
    new AlertDialog.Builder(this)
        .setTitle(t.source.name)
        .setItems(new String[] {"Start task", "Clear status", "Edit settings", "Edit output base name",
                      "Move up", "Move down", "Open source", "Open output", "Details", "Remove"},
            (d, i) -> {
              if (i == 0) {
                if (model.resetTask(t)) startQueue(t);
              } else if (i == 1) {
                model.resetTask(t);
              } else if (i == 2) {
                editOptions(t.options, t);
              } else if (i == 3) {
                editName(t);
              } else if (i == 4 || i == 5) {
                synchronized (model) {
                  int n = model.tasks.indexOf(t), target = n + (i == 4 ? -1 : 1);
                  if (n >= 0 && target >= 0 && target < model.tasks.size())
                    Collections.swap(model.tasks, n, target);
                }
              } else if (i == 6) {
                open(t.source.uri);
              } else if (i == 7) {
                if (t.output == null) toast("No output yet");
                else open(t.output);
              } else if (i == 8) {
                new AlertDialog.Builder(this).setTitle("Task details")
                    .setMessage(t.detail + (t.output == null ? "" : "\n\n" + t.output)
                        + (t.hash.isEmpty() ? "" : "\n\nSHA-256: " + t.hash))
                    .setPositiveButton("Close", null).show();
              } else if (i == 9) {
                model.removeTask(t);
              }
              refresh();
            })
        .show();
  }
  void editName(AppModel.Task t) {
    String filename = t.filename;
    int dot = filename.lastIndexOf('.');
    String base = dot > 0 ? filename.substring(0, dot) : filename;
    String ext = dot > 0 ? filename.substring(dot) : "";
    EditText e = new EditText(this);
    e.setSingleLine();
    e.setText(base);
    new AlertDialog.Builder(this)
        .setTitle("Output base name (extension is fixed)")
        .setView(e)
        .setPositiveButton("Save",
            (d, i) -> {
              String n = e.getText().toString().trim();
              if (n.isEmpty() || !n.equals(safeName(n)) || n.equals(".") || n.equals("..")) {
                toast("Enter a valid base name");
                return;
              }
              t.filename = n + ext;
              t.status = "Ready";
              t.progress = 0;
              t.detail = "";
              t.output = null;
              t.hash = "";
              refresh();
            })
        .setNegativeButton("Cancel", null)
        .show();
  }
  void preview(Uri u) {
    try {
      Intent i = new Intent(this, HdrPreviewActivity.class)
                     .setData(u)
                     .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
      startActivity(i);
    } catch (Exception e) {
      toast("HDR preview unavailable: " + AppModel.message(e));
    }
  }
  void open(Uri u) {
    try {
      Intent i = new Intent(Intent.ACTION_VIEW)
                     .setDataAndType(u, getContentResolver().getType(u))
                     .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
      startActivity(i);
    } catch (Exception e) {
      toast("No app can open this document");
    }
  }
  void toast(String s) {
    Toast.makeText(this, s, Toast.LENGTH_LONG).show();
  }
  void editOptions(Options initial, AppModel.Task task) {
    new Editor(initial, task).show();
  }
  final class Editor {
    Options value;
    final AppModel.Task task;
    final HashMap<Integer, Options> drafts = new HashMap<>();
    LinearLayout panel;
    Spinner formatControl, transferControl, gamutControl, scaleControl, channelsControl,
        peakControl, extractControl, compressionControl;
    EditText qualityControl, suffixControl, caption1Control, caption2Control;
    SeekBar qualitySlider;
    CheckBox losslessControl, exifControl, xmpControl, suffixCheck;
    boolean rebuilding;
    Editor(Options o, AppModel.Task t) {
      value = o.copy();
      task = t;
      if (t == null)
        value.useSuffix = prefs.getBoolean("useSuffixGlobal", value.useSuffix);
      drafts.put(value.format, value);
    }
    void show() {
      LinearLayout outer = new LinearLayout(MainActivity.this);
      outer.setOrientation(LinearLayout.VERTICAL);
      outer.setPadding(dp(20), dp(8), dp(20), dp(8));
      ScrollView scroll = new ScrollView(MainActivity.this);
      scroll.addView(outer);
      formatControl = new Spinner(MainActivity.this);
      formatControl.setAdapter(new ArrayAdapter<>(
          MainActivity.this, android.R.layout.simple_spinner_dropdown_item, Options.LABELS));
      formatControl.setSelection(value.format);
      outer.addView(formatControl);
      panel = new LinearLayout(MainActivity.this);
      panel.setOrientation(LinearLayout.VERTICAL);
      outer.addView(panel);
      render();
      formatControl.setOnItemSelectedListener(
          new android.widget.AdapterView.OnItemSelectedListener() {
            public void onNothingSelected(android.widget.AdapterView<?> p) {}
            public void onItemSelected(android.widget.AdapterView<?> p, View v, int n, long id) {
              if (n == value.format)
                return;
              try {
                read();
                drafts.put(value.format, value);
                boolean suffixOn = value.useSuffix;
                value = drafts.containsKey(n) ? drafts.get(n) : Options.load(prefs, n);
                value.useSuffix = suffixOn;
                render();
              } catch (Exception e) {
                toast(AppModel.message(e));
                formatControl.setSelection(value.format);
              }
            }
          });
      AlertDialog dialog = new AlertDialog.Builder(MainActivity.this)
                               .setTitle(task == null ? "Output settings" : "Edit queued task")
                               .setView(scroll)
                               .setPositiveButton("Save", null)
                               .setNeutralButton("Reset format", null)
                               .setNegativeButton("Cancel", null)
                               .create();
      dialog.setOnShowListener(x -> {
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
          try {
            read();
            if (task == null) {
              current = value.copy();
              prefs.edit()
                  .putInt("format", value.format)
                  .putBoolean("useSuffixGlobal", value.useSuffix)
                  .apply();
              for (Options d : drafts.values())
                prefs.edit().putString("options_" + d.format, d.save().toString()).apply();
              prefs.edit().putString("options_" + value.format, value.save().toString()).apply();
            } else {
              task.options = value.copy();
              task.filename = value.filename(task.source.name);
              task.status = "Ready";
              task.progress = 0;
              task.detail = "";
              task.output = null;
              task.hash = "";
            }
            dialog.dismiss();
            refresh();
            if (task == null) renderInline();
          } catch (Exception e) {
            toast(AppModel.message(e));
          }
        });
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
          boolean suffixOn = value.useSuffix;
          value = Options.defaults(value.format);
          value.useSuffix = suffixOn;
          drafts.put(value.format, value);
          render();
        });
      });
      dialog.show();
    }
    Spinner spinner(String label, String[] choices, int index) {
      TextView l = new TextView(MainActivity.this);
      l.setText(label);
      panel.addView(l);
      Spinner s = new Spinner(MainActivity.this);
      s.setAdapter(new ArrayAdapter<>(
          MainActivity.this, android.R.layout.simple_spinner_dropdown_item, choices));
      s.setSelection(Math.max(0, Math.min(index, choices.length - 1)));
      panel.addView(s);
      return s;
    }
    CheckBox check(String label, boolean b) {
      CheckBox c = new CheckBox(MainActivity.this);
      c.setText(label);
      c.setChecked(b);
      panel.addView(c);
      return c;
    }
    EditText input(String label, String text, boolean number) {
      TextView l = new TextView(MainActivity.this);
      l.setText(label);
      panel.addView(l);
      EditText e = new EditText(MainActivity.this);
      e.setSingleLine();
      if (number)
        e.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
      e.setText(text);
      panel.addView(e);
      return e;
    }
    void render() {
      rebuilding = true;
      panel.removeAllViews();
      transferControl = gamutControl = scaleControl = channelsControl = peakControl =
          extractControl = compressionControl = null;
      qualityControl = suffixControl = null;
      qualitySlider = null;
      losslessControl = exifControl = xmpControl = suffixCheck = null;
      int f = value.format;
      caption1Control = caption2Control = null;
      if (f == 7) {
        TextView frameInfo = new TextView(MainActivity.this);
        frameInfo.setText("HDR input: Ultra HDR JPEG output");
        panel.addView(frameInfo);
        caption1Control = input("Caption 1 (empty: camera from EXIF)", value.caption1, false);
        caption2Control = input("Caption 2 (empty: exposure from EXIF)", value.caption2, false);
      }
      boolean gm = value.gainmap();
      if (f == 1 || f == 3 || f == 5) {
        String[] choices = (f == 3 || f == 5) ? new String[] {"PQ / ST2084", "HLG", "Gain Map"}
                                              : new String[] {"PQ / ST2084", "HLG"};
        transferControl = spinner("Transfer / representation", choices, value.transfer);
        transferControl.setOnItemSelectedListener(
            new android.widget.AdapterView.OnItemSelectedListener() {
              public void onNothingSelected(android.widget.AdapterView<?> p) {}
              public void onItemSelected(android.widget.AdapterView<?> p, View v, int n, long id) {
                if (rebuilding || n == value.transfer)
                  return;
                try {
                  read();
                  value.transfer = n;
                  value.gamut = value.gainmap() ? "p3" : "rec2020";
                  value.suffix = Options.defaultSuffix(value.format, n);
                  render();
                } catch (Exception e) {
                  transferControl.setSelection(value.transfer);
                  toast(AppModel.message(e));
                }
              }
            });
      }
      if (f == 0 || f == 1 || f == 2 || f == 3 || f == 5 || f == 7) {
        String[] labels = (f == 0 || f == 7) ? new String[] {"Rec.709", "Display P3"}
            : gm                 ? new String[] {"Rec.709", "Display P3", "Rec.2020"}
                                 : new String[] {"Rec.2020", "Display P3"};
        int index = (f == 0 || f == 7) ? (value.gamut.equals("rec709") ? 0 : 1)
            : gm           ? (value.gamut.equals("rec709")      ? 0
                                     : value.gamut.equals("p3") ? 1
                                                                : 2)
                           : (value.gamut.equals("p3") ? 1 : 0);
        gamutControl = spinner("Gamut", labels, index);
      }
      if (gm) {
        scaleControl = spinner("Gain map resolution", new String[] {"Quarter", "Half", "Full"},
            value.scale == 4       ? 0
                : value.scale == 1 ? 2
                                   : 1);
        if (f == 0 || f == 7)
          channelsControl =
              spinner("Gain map channels", new String[] {"Mono", "RGB"}, value.channels);
        else {
          TextView t = new TextView(MainActivity.this);
          t.setText("Gain map channels: RGB");
          panel.addView(t);
        }
        peakControl = spinner("Target peak",
            new String[] {"Faithful / Auto", "1000 nits", "2000 nits", "4000 nits", "10000 nits"},
            value.peak);
      }
      if (f == 2) {
        TextView fixedTransfer = new TextView(MainActivity.this);
        fixedTransfer.setText("Transfer: PQ / ST2084");
        panel.addView(fixedTransfer);
        compressionControl =
            spinner("TIFF compression", new String[] {"Deflate", "None"}, value.compressed ? 0 : 1);
        compressionControl.setOnItemSelectedListener(
            new android.widget.AdapterView.OnItemSelectedListener() {
              public void onNothingSelected(android.widget.AdapterView<?> p) {}
              public void onItemSelected(android.widget.AdapterView<?> p, View v, int n, long id) {
                if (qualityControl != null)
                  qualityControl.setEnabled(n == 0);
                if (qualitySlider != null)
                  qualitySlider.setEnabled(n == 0);
              }
            });
      }
      if (f == 6) {
        extractControl = spinner(
            "Gain map export", new String[] {"Original", "PNG", "TIFF", "JPEG"}, value.extraction);
        extractControl.setOnItemSelectedListener(
            new android.widget.AdapterView.OnItemSelectedListener() {
              public void onNothingSelected(android.widget.AdapterView<?> p) {}
              public void onItemSelected(android.widget.AdapterView<?> p, View v, int n, long id) {
                if (qualityControl != null)
                  qualityControl.setEnabled(n == 3);
                if (qualitySlider != null)
                  qualitySlider.setEnabled(n == 3);
              }
            });
      }
      if (f == 3 || f == 4) {
        losslessControl = check("Lossless", value.lossless);
        losslessControl.setOnCheckedChangeListener((b, on) -> {
          if (qualityControl != null)
            qualityControl.setEnabled(!on);
          if (qualitySlider != null)
            qualitySlider.setEnabled(!on);
        });
      } else if (f == 7) {
        losslessControl = check("Lossless", false);
        losslessControl.setEnabled(false);
      } else if (f == 1 || f == 2) {
        TextView fixed = new TextView(MainActivity.this);
        fixed.setText("Lossless");
        panel.addView(fixed);
      }
      qualityControl = input(f == 1 || f == 2 ? "Compression level (1–9)"
              : f == 0                        ? "JPEG base and gain map quality (1–100)"
                                              : "Quality (1–100)",
          Integer.toString(value.quality), true);
      qualityControl.setEnabled((f != 2 || value.compressed) && (f != 6 || value.extraction == 3)
          && ((f != 3 && f != 4) || !value.lossless));
      qualitySlider = new SeekBar(MainActivity.this);
      qualitySlider.setMax(f == 1 || f == 2 ? 9 : 100);
      qualitySlider.setMin(1);
      qualitySlider.setProgress(value.quality);
      qualitySlider.setEnabled(qualityControl.isEnabled());
      panel.addView(qualitySlider);
      qualitySlider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
        public void onStartTrackingTouch(SeekBar b) {}
        public void onStopTrackingTouch(SeekBar b) {}
        public void onProgressChanged(SeekBar b, int progress, boolean user) {
          if (user)
            qualityControl.setText(Integer.toString(progress));
        }
      });
      qualityControl.addTextChangedListener(new android.text.TextWatcher() {
        public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
        public void onTextChanged(CharSequence s, int start, int before, int count) {
          try {
            int n = Integer.parseInt(s.toString());
            if (n >= 1 && n <= qualitySlider.getMax())
              qualitySlider.setProgress(n);
          } catch (NumberFormatException ignored) {
          }
        }
        public void afterTextChanged(android.text.Editable e) {}
      });
      exifControl = check("Copy EXIF", f != 7 && value.exif);
      xmpControl = check("Copy XMP", f != 7 && value.xmp);
      exifControl.setEnabled(f != 7);
      xmpControl.setEnabled(f != 7);
      suffixCheck = check("Use suffix", value.useSuffix);
      suffixControl = input("Output suffix", value.suffix, false);
      rebuilding = false;
    }
    void read() {
      Options next = value.copy();
      int f = value.format;
      int q = Integer.parseInt(qualityControl.getText().toString());
      int max = (f == 1 || f == 2) ? 9 : 100;
      if (q < 1 || q > max)
        throw new IllegalArgumentException("Value must be between 1 and " + max);
      next.quality = q;
      if (caption1Control != null) {
        next.caption1 = caption1Control.getText().toString();
        next.caption2 = caption2Control.getText().toString();
      }
      boolean gm = value.gainmap();
      if (gamutControl != null) {
        int n = gamutControl.getSelectedItemPosition();
        next.gamut = (f == 0 || f == 7) ? new String[] {"rec709", "p3"}[n]
            : gm             ? new String[] {"rec709", "p3", "rec2020"}[n]
                             : new String[] {"rec2020", "p3"}[n];
      }
      if (transferControl != null)
        next.transfer = transferControl.getSelectedItemPosition();
      if (scaleControl != null)
        next.scale = new int[] {4, 2, 1}[scaleControl.getSelectedItemPosition()];
      if (channelsControl != null)
        next.channels = channelsControl.getSelectedItemPosition();
      if (peakControl != null)
        next.peak = peakControl.getSelectedItemPosition();
      if (extractControl != null)
        next.extraction = extractControl.getSelectedItemPosition();
      if (compressionControl != null)
        next.compressed = compressionControl.getSelectedItemPosition() == 0;
      if (losslessControl != null)
        next.lossless = losslessControl.isChecked();
      next.exif = exifControl.isChecked();
      next.xmp = xmpControl.isChecked();
      next.useSuffix = suffixCheck.isChecked();
      next.suffix = suffixControl.getText().toString();
      if (next.suffix.matches(".*[\\\\/:*?\"<>|\\p{Cntrl}].*"))
        throw new IllegalArgumentException("Suffix contains an invalid character");
      value = next;
    }
  }
}
