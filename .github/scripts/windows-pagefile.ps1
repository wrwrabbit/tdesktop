param(
    [string] $Path = 'D:\pagefile-extra.sys',
    [long] $SizeGB = 40
)

$source = @'
using System;
using System.Runtime.InteropServices;

public static class ExtraPageFile {
    [StructLayout(LayoutKind.Sequential)]
    struct UNICODE_STRING {
        public ushort Length;
        public ushort MaximumLength;
        public IntPtr Buffer;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct TOKEN_PRIVILEGES {
        public uint PrivilegeCount;
        public long Luid;
        public uint Attributes;
    }

    [DllImport("ntdll.dll")]
    static extern int NtCreatePagingFile(ref UNICODE_STRING name, ref long minimum, ref long maximum, uint priority);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool LookupPrivilegeValue(string system, string name, out long luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TOKEN_PRIVILEGES state, int length, IntPtr previous, IntPtr returned);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();

    public static int Create(string path, long bytes) {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) {
            return -1;
        }
        var state = new TOKEN_PRIVILEGES { PrivilegeCount = 1, Attributes = 2 };
        if (!LookupPrivilegeValue(null, "SeCreatePagefilePrivilege", out state.Luid)) {
            return -2;
        }
        if (!AdjustTokenPrivileges(token, false, ref state, 0, IntPtr.Zero, IntPtr.Zero)) {
            return -3;
        }
        var nt = "\\??\\" + path;
        var name = new UNICODE_STRING {
            Length = (ushort)(nt.Length * 2),
            MaximumLength = (ushort)(nt.Length * 2 + 2),
            Buffer = Marshal.StringToHGlobalUni(nt)
        };
        long minimum = bytes;
        long maximum = bytes;
        return NtCreatePagingFile(ref name, ref minimum, ref maximum, 0);
    }
}
'@

Add-Type -TypeDefinition $source
$status = [ExtraPageFile]::Create($Path, $SizeGB * 1GB)
Write-Host ("[pagefile] NtCreatePagingFile returned 0x{0:X}" -f $status)
Get-CimInstance Win32_PageFileUsage | ForEach-Object { Write-Host "[pagefile] $($_.Name) $($_.AllocatedBaseSize)MB" }
if ($status -ne 0) {
    exit 1
}
