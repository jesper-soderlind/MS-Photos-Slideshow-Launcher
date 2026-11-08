using System.Runtime.InteropServices;
using System.Text;
using Windows.Foundation.Collections;
using Windows.System;

namespace PhotosSlideshow;

/// <summary>
/// Launches Microsoft Photos in slideshow mode for a folder or a list of files,
/// then injects PhotosSlideshowHook.dll into the Photos process so the slideshow
/// duration setting (and any other runtime patches) is applied.
///
/// Usage:
///   PhotosSlideshow.exe "C:\path\to\folder" [-r|--recursive] [-s|--shuffle] [--windowed] [--duration <ms>]
///   PhotosSlideshow.exe --files "C:\a.jpg;C:\b.png" [same flags]
///   PhotosSlideshow.exe --duration <ms> | --shuffle | --recursive   # persist setting without launching
/// </summary>
public static class Program
{
    private const string SlideshowUri = "ms-photos:slideshow";
    private const string PackageFamilyName = "Microsoft.Windows.Photos_8wekyb3d8bbwe";
    private const int MaxFiles = 900;
    private const int MaxPayloadBytes = 90 * 1024;

    private static readonly HashSet<string> MediaExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".jpg", ".jpeg", ".jpe", ".png", ".gif", ".bmp", ".dib", ".tif", ".tiff",
        ".webp", ".heic", ".heif", ".hif", ".ico", ".cur",
        ".dng", ".cr2", ".cr3", ".nef", ".nrw", ".orf", ".rw2", ".arw", ".pef",
        ".srw", ".x3f", ".raf", ".erf", ".kdc", ".mrw", ".raw",
        ".mp4", ".m4v", ".mov", ".wmv", ".avi", ".mkv", ".mpg", ".mpeg", ".mts",
        ".m2ts", ".3gp", ".3g2", ".webm", ".flv", ".vob", ".mxf", ".asf", ".divx",
        ".ogv", ".rm", ".rmvb", ".m2v", ".wm", ".mod", ".tod", ".wtv", ".f4v"
    };

    public static async Task<int> Main(string[] args)
    {
        // Fresh hook.log for this launch - the hook DLL appends to it during
        // the session, but each exe run starts it over. Best-effort: a
        // still-running Photos process may briefly hold the file open.
        try { File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "hook.log"), ""); }
        catch { }

        bool recursiveArg = false;
        bool shuffleArg = false;
        bool windowed = false;
        int? durationMs = null;
        string? folder = null;
        string? fileList = null;

        var positional = new List<string>();
        for (int i = 0; i < args.Length; i++)
        {
            var a = args[i];
            if (a.Equals("-r", StringComparison.OrdinalIgnoreCase) ||
                a.Equals("--recursive", StringComparison.OrdinalIgnoreCase)) { recursiveArg = true; continue; }
            if (a.Equals("-s", StringComparison.OrdinalIgnoreCase) ||
                a.Equals("--shuffle", StringComparison.OrdinalIgnoreCase)) { shuffleArg = true; continue; }
            if (a.Equals("--windowed", StringComparison.OrdinalIgnoreCase)) { windowed = true; continue; }
            if (a.Equals("--duration", StringComparison.OrdinalIgnoreCase) || a.Equals("-d", StringComparison.OrdinalIgnoreCase))
            {
                if (i + 1 < args.Length && int.TryParse(args[++i], out int d)) durationMs = d;
                continue;
            }
            if (a.Equals("--files", StringComparison.OrdinalIgnoreCase))
            {
                if (i + 1 < args.Length) fileList = args[++i];
                continue;
            }
            positional.Add(a);
        }

        foreach (var p in positional)
        {
            if (Directory.Exists(p)) folder = p;
            else if (File.Exists(p)) fileList = p;
        }

        bool launching = folder != null || fileList != null;

        // --shuffle / --recursive / --duration persist to settings.ini only
        // when NOT launching a slideshow; with a folder/files they apply to
        // this run only.
        if (shuffleArg && !launching)
        {
            WriteSetting("Shuffle", "1");
            Console.WriteLine("Shuffle enabled (persisted).");
        }
        bool shuffle = launching && (shuffleArg || ReadSettingBool("Shuffle"));

        if (recursiveArg && !launching)
        {
            WriteSetting("Recursive", "1");
            Console.WriteLine("Recursive enabled (persisted).");
        }
        bool recursive = launching && (recursiveArg || ReadSettingBool("Recursive"));

        int? sessionDurationMs = null;
        if (durationMs.HasValue)
        {
            if (durationMs.Value <= 0)
            {
                Console.Error.WriteLine("Warning: --duration must be a positive number of ms; ignored.");
            }
            else if (launching)
            {
                sessionDurationMs = durationMs.Value;
                Console.WriteLine($"Slideshow duration {durationMs.Value} ms for this launch (not persisted).");
            }
            else
            {
                WriteSetting("DurationMs", durationMs.Value.ToString());
                Console.WriteLine($"Slideshow duration set to {durationMs.Value} ms (persisted).");
            }
        }

        List<string> files;
        if (fileList != null)
        {
            files = fileList.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                            .Where(File.Exists).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        }
        else if (folder != null)
        {
            files = EnumerateMediaFiles(folder, recursive);
        }
        else
        {
            if (durationMs.GetValueOrDefault() > 0 || shuffleArg || recursiveArg)
                return 0; // settings-only run; values already persisted above
            Console.Error.WriteLine("Usage: PhotosSlideshow.exe \"<folder>\" [-r|--recursive] [-s|--shuffle] [--windowed] [--duration <ms>]");
            Console.Error.WriteLine("       PhotosSlideshow.exe --files \"C:\\a.jpg;C:\\b.png\" [-s|--shuffle] [--windowed] [--duration <ms>]");
            Console.Error.WriteLine("       PhotosSlideshow.exe --duration <ms> | --shuffle | --recursive   # persist setting without launching");
            return 2;
        }

        if (files.Count == 0)
        {
            Console.Error.WriteLine("No media files found.");
            return 3;
        }

        // cap to protocol payload limits
        var capped = new List<string>();
        int payload = 0;
        foreach (var f in files)
        {
            int cost = Encoding.Unicode.GetByteCount(f) + 4;
            if (capped.Count >= MaxFiles || payload + cost > MaxPayloadBytes) break;
            capped.Add(f);
            payload += cost;
        }
        if (capped.Count < files.Count)
            Console.Error.WriteLine($"Warning: {files.Count - capped.Count} files omitted (protocol payload limit).");

        if (shuffle && capped.Count > 1)
        {
            // Fisher-Yates
            for (int i = capped.Count - 1; i > 0; i--)
            {
                int j = Random.Shared.Next(i + 1);
                (capped[i], capped[j]) = (capped[j], capped[i]);
            }
        }

        // Per-launch duration override for the hook; delete any stale file first
        // so a previous crashed run can't leak into this one.
        if (sessionDurationMs.HasValue)
        {
            try { WriteSessionDuration(sessionDurationMs.Value); }
            catch (Exception ex) { Console.Error.WriteLine($"Warning: could not write session duration: {ex.Message}"); }
        }
        else
        {
            DeleteSessionDuration();
        }

        var valueSet = new ValueSet
        {
            ["Sort"] = "Name",
            ["SortDirection"] = "Ascending",
            ["FolderDepth"] = "NeverInclude",
            ["EntryPoint"] = "GalleryContext",
            ["FilePaths"] = capped.ToArray(),
            ["SlideshowFilePathsStartIndex"] = 0u,
            ["WindowsActivationKind"] = "Collection",
        };

        var options = new LauncherOptions { TargetApplicationPackageFamilyName = PackageFamilyName };
        string status;
        try
        {
            var result = await Launcher.LaunchUriAsync(new Uri(SlideshowUri), options, valueSet);
            status = result.ToString() ?? "?";
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Failed to launch Photos slideshow: {ex.Message}");
            return 4;
        }
        Console.WriteLine($"Launched ms-photos:slideshow with {capped.Count} files. Status={status}");

        // windowed restore polls for the window itself, so run it concurrently
        // with injection - F11 works even during the "building slideshow" screen
        Task? restoreTask = windowed ? Task.Run(RestorePhotosWindows) : null;
        InjectIntoPhotos();
        // give hook threads a moment to read session_duration.txt, then remove it
        Thread.Sleep(750);
        restoreTask?.Wait();
        DeleteSessionDuration();
        return 0;
    }

    // ---------------- settings.ini / session override ----------------

    private static string IniPath => Path.Combine(AppContext.BaseDirectory, "settings.ini");
    private static string SessionDurationPath => Path.Combine(AppContext.BaseDirectory, "session_duration.txt");

    private static void WriteSetting(string key, string value)
    {
        // settings.ini lives next to PhotosSlideshow.exe, where the hook DLL
        // also sits - the hook resolves its own module directory at runtime.
        if (!WritePrivateProfileStringW("Slideshow", key, value, IniPath))
            Console.Error.WriteLine($"Warning: could not write {key} to {IniPath}");
    }

    private static bool ReadSettingBool(string key)
    {
        var buf = new StringBuilder(16);
        GetPrivateProfileStringW("Slideshow", key, "0", buf, buf.Capacity, IniPath);
        return buf.ToString() is "1" or "y" or "Y" or "true" or "True";
    }

    // Per-launch duration override consumed once by the hook DLL at injection
    // time - never written to settings.ini, so it does not persist.
    private static void WriteSessionDuration(int ms)
    {
        File.WriteAllText(SessionDurationPath, ms.ToString());
    }

    private static void DeleteSessionDuration()
    {
        try { File.Delete(SessionDurationPath); } catch { }
    }

    // ---------------- file enumeration ----------------

    private static List<string> EnumerateMediaFiles(string folder, bool recursive)
    {
        var files = new List<string>();
        var opt = recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly;
        IEnumerable<string> paths;
        try { paths = Directory.EnumerateFiles(folder, "*", opt); }
        catch { return files; }

        // EnumerateFiles is deferred - unreadable dirs (e.g. denied subfolders
        // under -r) throw here, mid-iteration, not at the call above.
        try
        {
            foreach (var p in paths)
            {
                if (MediaExtensions.Contains(Path.GetExtension(p)))
                    files.Add(Path.GetFullPath(p));
            }
        }
        catch { /* skip unreadable entries, keep what we have */ }
        files.Sort(StringComparer.OrdinalIgnoreCase);
        return files;
    }

    // ---------------- windowed mode ----------------

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    private static void RestorePhotosWindows()
    {
        try { RestorePhotosWindowsCore(); }
        catch (Exception ex) { Console.Error.WriteLine($"Windowed restore failed: {ex.Message}"); }
    }

    private static void RestorePhotosWindowsCore()
    {
        // Photos opens the slideshow fullscreen. Its F11 accelerator toggles
        // fullscreen with proper internal state handling, so we focus the
        // window and send a real F11 keystroke - but only if the window is
        // currently borderless (style lacks WS_CAPTION etc.).
        var deadline = DateTime.UtcNow.AddSeconds(20);
        bool done = false;

        while (DateTime.UtcNow < deadline && !done)
        {
            // Photos.exe may not have spawned yet on the first poll - refresh
            // the pid set every iteration rather than snapshotting once.
            var pids = new HashSet<int>(System.Diagnostics.Process.GetProcessesByName("Photos").Select(p => p.Id));
            var hwnds = new List<IntPtr>();
            EnumWindows((hw, _) =>
            {
                uint pid = 0;
                GetWindowThreadProcessId(hw, out pid);
                if (pids.Contains((int)pid) && IsWindowVisible(hw))
                    hwnds.Add(hw);
                return true;
            }, IntPtr.Zero);

            foreach (var hw in hwnds)
            {
                long style = GetWindowLong(hw, GWL_STYLE);
                const long wsCaptionBits = 0x00CF0000; // CAPTION|SYSMENU|THICKFRAME|MIN|MAX boxes
                if ((style & wsCaptionBits) == wsCaptionBits) continue; // already windowed

                if (SetForegroundWindow(hw))
                {
                    Thread.Sleep(300);
                    keybd_event(VK_F11, 0, 0, UIntPtr.Zero);
                    Thread.Sleep(50);
                    keybd_event(VK_F11, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
                    done = true;
                    Console.WriteLine("Sent F11 to Photos window (exit fullscreen).");
                }
            }
            if (!done) Thread.Sleep(200);
        }
        if (!done) Console.Error.WriteLine("No fullscreen Photos window found for F11.");
    }

    // ---------------- DLL injection ----------------

    private static void InjectIntoPhotos()
    {
        string dllPath = Path.Combine(AppContext.BaseDirectory, "PhotosSlideshowHook.dll");
        if (!File.Exists(dllPath))
        {
            // fallback for running the exe outside its usual folder
            dllPath = Path.Combine(Environment.CurrentDirectory, "PhotosSlideshowHook.dll");
        }
        if (!File.Exists(dllPath))
        {
            Console.Error.WriteLine("PhotosSlideshowHook.dll not found next to PhotosSlideshow.exe; skipping injection.");
            return;
        }

        // give Photos a moment to spawn, then inject into every Photos.exe process
        // (there may be a spare process; the one running the slideshow will pick it up)
        var deadline = DateTime.UtcNow.AddSeconds(20);
        // key on (pid, start time) so a recycled PID can't make us skip a new process
        var injected = new HashSet<(int, DateTime)>();
        DateTime lastNew = DateTime.UtcNow;
        while (DateTime.UtcNow < deadline)
        {
            foreach (var p in System.Diagnostics.Process.GetProcessesByName("Photos"))
            {
                DateTime start;
                try { start = p.StartTime; } catch { start = DateTime.MinValue; }
                var key = (p.Id, start);
                if (injected.Add(key))
                {
                    lastNew = DateTime.UtcNow;
                    bool ok = TryInject(p.Id, dllPath);
                    Console.WriteLine($"Inject pid={p.Id}: {(ok ? "ok" : "failed")}");
                }
            }
            // stop once we've injected somewhere and no new Photos process appeared for 4s
            if (injected.Count > 0 && (DateTime.UtcNow - lastNew).TotalSeconds > 4)
                break;
            Thread.Sleep(400);
        }
    }

    private static bool TryInject(int pid, string dllPath)
    {
        IntPtr hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, false, pid);
        if (hProc == IntPtr.Zero) return false;
        try
        {
            IntPtr remote = VirtualAllocEx(hProc, IntPtr.Zero, (uint)((dllPath.Length + 1) * 2), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (remote == IntPtr.Zero) return false;
            var bytes = Encoding.Unicode.GetBytes(dllPath + "\0");
            if (!WriteProcessMemory(hProc, remote, bytes, (uint)bytes.Length, out _))
            {
                VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
                return false;
            }
            IntPtr loadLib = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
            if (loadLib == IntPtr.Zero) return false;
            IntPtr thread = CreateRemoteThread(hProc, IntPtr.Zero, 0, loadLib, remote, 0, out _);
            if (thread == IntPtr.Zero) { VirtualFreeEx(hProc, remote, 0, MEM_RELEASE); return false; }
            WaitForSingleObject(thread, 10000);
            // LoadLibraryW's return value (the module HMODULE, 0 on failure)
            // surfaces as the remote thread's exit code.
            GetExitCodeThread(thread, out uint exitCode);
            CloseHandle(thread);
            VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
            return exitCode != 0;
        }
        finally { CloseHandle(hProc); }
    }

    private const uint PROCESS_CREATE_THREAD = 0x0002;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint PROCESS_VM_OPERATION = 0x0008;
    private const uint PROCESS_VM_WRITE = 0x0020;
    private const uint PROCESS_VM_READ = 0x0010;
    private const uint MEM_COMMIT = 0x1000;
    private const uint MEM_RESERVE = 0x2000;
    private const uint MEM_RELEASE = 0x8000;
    private const uint PAGE_READWRITE = 0x04;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr hProc, IntPtr addr, uint size, uint type, uint protect);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr hProc, IntPtr addr, byte[] buf, uint size, out int written);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(IntPtr hProc, IntPtr attr, uint stack, IntPtr start, IntPtr param, uint flags, out int tid);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GetModuleHandle(string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string proc);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFreeEx(IntPtr hProc, IntPtr addr, uint size, uint type);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeThread(IntPtr thread, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool WritePrivateProfileStringW(string app, string key, string val, string file);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetPrivateProfileStringW(string app, string key, string def, StringBuilder ret, int size, string file);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] private static extern long GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll")] private static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    private const int GWL_STYLE = -16;
    private const byte VK_F11 = 0x7A;
    private const uint KEYEVENTF_KEYUP = 0x0002;
}
