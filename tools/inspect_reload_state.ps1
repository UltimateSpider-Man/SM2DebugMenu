[CmdletBinding()]
param([Parameter(Mandatory = $true)][int]$GamePid)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell for the read-only inspector.' }
$project = Split-Path -Parent $PSScriptRoot
$gamePath = Join-Path (Split-Path -Parent $project) 'Spider-Man2.exe'
. (Join-Path $PSScriptRoot 'supported_builds.ps1')
$null = Get-SM2SupportedGameHash $gamePath
$process = Get-Process -Id $GamePid -ErrorAction Stop
if (-not [String]::Equals($process.Path, $gamePath, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected game process path.' }
$image = [uint64]$process.MainModule.BaseAddress.ToInt64()
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public sealed class SM2ReadOnlyReload : IDisposable {
 [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll", SetLastError=true)] static extern bool ReadProcessMemory(IntPtr process,IntPtr address,byte[] bytes,UIntPtr size,out UIntPtr read);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr process);
 readonly IntPtr process;
 public SM2ReadOnlyReload(uint pid) { process=OpenProcess(0x1010,false,pid); if(process==IntPtr.Zero)throw new Exception("Read-only process open failed."); }
 public void Dispose() { if(process!=IntPtr.Zero)CloseHandle(process); }
 public byte[] Bytes(ulong address,int length) {
  if(address<65536 || length<1 || length>1048576 || address>0x7fffffffffffUL-(ulong)length)throw new Exception("Read bounds invalid.");
  var bytes=new byte[length]; UIntPtr read;
  if(!ReadProcessMemory(process,new IntPtr((long)address),bytes,(UIntPtr)(uint)length,out read) || read.ToUInt64()!=(ulong)length)throw new Exception("Unreadable native state.");
  return bytes;
 }
 public ulong U64(ulong a) {return BitConverter.ToUInt64(Bytes(a,8),0);}
 public uint U32(ulong a) {return BitConverter.ToUInt32(Bytes(a,4),0);}
 public int I32(ulong a) {return BitConverter.ToInt32(Bytes(a,4),0);}
 public byte U8(ulong a) {return Bytes(a,1)[0];}
 public string Name(ulong a) {
  var bytes=new List<byte>();
  for(int i=0;i<512;i++){var c=U8(a+(ulong)i);if(c==0)return Encoding.UTF8.GetString(bytes.ToArray());bytes.Add(c);}
  throw new Exception("Checkpoint name exceeded bounds.");
 }
 public uint Crc(string name) {uint crc=0xedb88320;foreach(byte c in Encoding.UTF8.GetBytes(name)){crc^=c;for(int i=0;i<8;i++)crc=(crc>>1)^((crc&1)!=0?0xedb88320U:0);}return crc;}
}
'@
$reader = New-Object SM2ReadOnlyReload([uint32]$GamePid)
$data = [ordered]@{}
$guards = [ordered]@{}
$checkpointRows = @()
$failure = $null
try {
    $manager = $reader.U64($image + 0xc2266a8)
    $data.manager = $manager
    $guards.manager_identity = $manager -eq ($image + 0xb26b150) -and $reader.U64($manager) -eq ($image + 0x547d668)
    $data.loaded_level = $reader.U64($manager + 0x10)
    $data.manager_ready = $reader.U8($manager + 0x430665)
    $data.manager_state = $reader.I32($manager + 0x430628)
    $data.pending_instances = $reader.I32($manager + 0x199fc0)
    $data.pending_deletions = $reader.I32($manager + 0x319f68)
    $guards.manager_stable = $data.loaded_level -ne 0 -and $data.manager_ready -eq 1 -and $data.manager_state -eq 22 -and
        $data.pending_instances -eq 0 -and $data.pending_deletions -eq 0
    $mode = $reader.U64($image + 0xc21d330)
    $data.mode_value = $reader.I32($mode + 0x10)
    $guards.mode_active = $data.mode_value -eq 9
    $handle = $reader.U32($image + 0xc21d630)
    $table = $reader.U64($image + 0xc0bebe0)
    $count = $reader.I32($image + 0xc0bec00)
    $index = $handle -band 0xfffff
    $generation = $handle -shr 20
    $data.kernel_handle = $handle
    $guards.handle_valid = $generation -ne 0 -and $index -lt $count -and $count -le 0x100000
    if (-not $guards.handle_valid) { throw 'Kernel handle is not active.' }
    $entry = $table + [uint64]$index * 16
    $kernel = $reader.U64($entry)
    $data.kernel = $kernel
    $guards.kernel_identity = $reader.U32($entry + 8) -eq $generation -and $reader.U64($kernel) -eq ($image + 0x83c0ef0)
    $data.kernel_owner = $reader.U64($kernel + 0x10)
    $data.kernel_state = $reader.U8($kernel + 0x6308)
    $data.level_asset = $reader.U64($kernel + 0x58)
    $data.current_checkpoint_key = $reader.U32($kernel + 0x62c0)
    $guards.kernel_active = $data.kernel_owner -ne 0 -and $data.kernel_state -eq 3 -and ($data.level_asset -shr 63) -eq 1
    $records = $reader.U64($manager + 0x41ff50)
    $recordCount = $reader.I32($manager + 0x41ff58)
    $data.checkpoint_count = $recordCount
    if ($recordCount -lt 0 -or $recordCount -gt 4096) { throw 'Checkpoint table exceeds inspection bounds.' }
    for ($i = 0; $i -lt $recordCount; $i++) {
        $record = $records + [uint64]$i * 72
        $key = $reader.U32($record + 0x28)
        $name = $reader.Name($reader.U64($record + 0x20))
        $checkpointRows += [pscustomobject]@{ index = $i; key = $key; name = $name; crc_matches = $reader.Crc($name) -eq $key }
    }
    $guards.checkpoint_name_keys = @($checkpointRows | Where-Object { -not $_.crc_matches }).Count -eq 0
    $save = $reader.U64($image + 0xc0bd2d8)
    $data.save_system = $save
    $guards.save_identity = $save -ne 0 -and $reader.U64($save) -eq ($image + 0x54a1ca0)
    $guards.snapshot_stable = $reader.U64($image + 0xc2266a8) -eq $manager -and $reader.U32($image + 0xc21d630) -eq $handle -and
        $reader.U64($image + 0xc0bebe0) -eq $table -and $reader.U64($entry) -eq $kernel -and $reader.U32($entry + 8) -eq $generation -and
        $reader.U64($kernel + 0x58) -eq $data.level_asset -and $reader.U32($kernel + 0x62c0) -eq $data.current_checkpoint_key -and
        $reader.U64($manager + 0x41ff50) -eq $records -and $reader.I32($manager + 0x41ff58) -eq $recordCount
} catch { $failure = $_.Exception.Message } finally { $reader.Dispose() }
$report = [ordered]@{
    schema = 1; generated_utc = [DateTime]::UtcNow.ToString('o'); game_pid = $GamePid
    process_path = $process.Path; image_base = $image; read_only = $true; engine_calls = 0
    data = $data; guards = $guards; failure = $failure; checkpoints = $checkpointRows
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $project 'verification\live-reload-state.json') -Encoding UTF8
[pscustomobject]@{ game_pid = $GamePid; data = $data; guards = $guards; failure = $failure } | ConvertTo-Json -Depth 6
