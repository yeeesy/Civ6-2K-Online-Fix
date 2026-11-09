// Embedded in the portable launcher. Only a signed release can replace the launcher.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;

internal static class UpdateAgent {
    internal const string Repo = "yeeesy/Civ6-2K-Online-Fix";
    internal const string Exe = "Civ6_2K_Online_Fix.exe";
    const int MaxZip = 32*1024*1024, MaxExe = 16*1024*1024;
    static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
    static readonly string Stage = AppDomain.CurrentDomain.BaseDirectory;
    static string Staged(string name) {return Path.Combine(Stage,name);}
    static void CheckStage() {
        var dir=new DirectoryInfo(Stage);
        var root=Path.GetFullPath(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                              "Civ6_2K_Online_Fix","Updates"));
        Require(dir.Parent!=null && Same(dir.Parent.FullName,root) &&
                Regex.IsMatch(dir.Name,@"\Ajob-[0-9a-f]{32}\z"));
        for(var d=dir;d!=null;d=d.Parent) Require((d.Attributes&FileAttributes.ReparsePoint)==0);
    }
    internal static bool Stable(string s) {
        if(s==null || !Regex.IsMatch(s,@"\A(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,4})\z",RegexOptions.CultureInvariant)) return false;
        Version v; return Version.TryParse(s,out v) && v.Major<=65535 && v.Minor<=65535 && v.Build<=65535;
    }
    internal static bool Newer(string next,string current) {
        if(!Stable(next) || current==null) return false;
        var part=current.Split(new[]{'-'},2);
        if(!Stable(part[0]) || (part.Length==2 && !Regex.IsMatch(part[1],@"\A[0-9A-Za-z.-]+\z")))return false;
        int c=new Version(next).CompareTo(new Version(part[0]));
        return c>0 || (c==0 && part.Length==2);
    }
    static bool Hex(string s) {return Regex.IsMatch(s,@"\A[0-9A-Fa-f]{64}\z");}
    static string Digest(Stream s) {using(var h=SHA256.Create())return BitConverter.ToString(h.ComputeHash(s)).Replace("-","");}
    internal static string HashFile(string p) {using(var f=File.OpenRead(p))return Digest(f);}
    static bool Same(string a,string b) {return String.Equals(a,b,StringComparison.OrdinalIgnoreCase);}
    static void Require(bool ok) {if(!ok)throw new InvalidDataException("update validation failed");}
    internal sealed class Manifest {internal string Version,ZipHash,ExeHash; internal long ZipSize,ExeSize;}
    internal static Manifest Verify(string envelope,string version,string xml) {
        Require(Stable(version) && envelope.Length<=32768 && xml.StartsWith("<RSAKeyValue>",StringComparison.Ordinal));
        var lines=envelope.TrimEnd('\r','\n').Replace("\r\n","\n").Split('\n');
        Require(lines.Length==2);
        var body=Convert.FromBase64String(lines[0]);var signature=Convert.FromBase64String(lines[1]);
        using(var rsa=new RSACryptoServiceProvider(new CspParameters(24){Flags=CspProviderFlags.CreateEphemeralKey})) {
            rsa.PersistKeyInCsp=false; rsa.FromXmlString(xml);
            Require(rsa.KeySize>=3072 && rsa.VerifyData(body,CryptoConfig.MapNameToOID("SHA256"),signature));
        }
        var v=Utf8.GetString(body).Split('\n');
        Require(v.Length==7 && v[0]=="CIV6-UPDATE/1" && v[1]==Repo && v[2]==version && Hex(v[4]) && Hex(v[6]));
        long zip,exe;
        Require(long.TryParse(v[3],NumberStyles.None,CultureInfo.InvariantCulture,out zip) && zip>0 && zip<=MaxZip);
        Require(long.TryParse(v[5],NumberStyles.None,CultureInfo.InvariantCulture,out exe) && exe>0 && exe<=MaxExe);
        return new Manifest{Version=version,ZipSize=zip,ZipHash=v[4],ExeSize=exe,ExeHash=v[6]};
    }
    static string Key() {
        using(var r=new StreamReader(Assembly.GetExecutingAssembly().GetManifestResourceStream("PublisherKey"),Utf8))
            return r.ReadToEnd().Trim();
    }
    static void BoundedCopy(Stream input,Stream output,long limit) {
        var buf=new byte[65536];long total=0;int n;
        while((n=input.Read(buf,0,buf.Length))!=0){total+=n;Require(total<=limit);output.Write(buf,0,n);}
    }
    static void Download(Uri uri,bool api,Stream output,long limit) {
        var watch=Stopwatch.StartNew();
        for(int redirect=0;redirect<6;redirect++){
            Require(uri.Scheme=="https" && uri.IsDefaultPort && uri.UserInfo=="" && (api?uri.Host=="api.github.com":
                uri.Host=="github.com" || uri.Host=="release-assets.githubusercontent.com" || uri.Host=="objects.githubusercontent.com"));
            var q=(HttpWebRequest)WebRequest.Create(uri);q.AllowAutoRedirect=false;q.Timeout=12000;q.ReadWriteTimeout=12000;
            q.UserAgent="Civ6-2K-Online-Fix-Updater";q.UseDefaultCredentials=false;q.AutomaticDecompression=DecompressionMethods.None;
            using(var res=(HttpWebResponse)q.GetResponse()){
                int code=(int)res.StatusCode;
                if(code==301 || code==302 || code==303 || code==307 || code==308){uri=new Uri(uri,res.Headers["Location"]);continue;}
                Require(code==200 && res.ContentLength<=limit);
                using(var stream=res.GetResponseStream()){
                    var buf=new byte[65536];long total=0;int n;
                    while((n=stream.Read(buf,0,buf.Length))!=0){total+=n;Require(total<=limit && watch.Elapsed.TotalSeconds<120);output.Write(buf,0,n);}
                }
                return;
            }
        }
        throw new InvalidDataException();
    }
    static string DownloadText(Uri u,bool api,int max) {using(var mem=new MemoryStream()){Download(u,api,mem,max);return Utf8.GetString(mem.ToArray());}}
    static string ProductVersion(string target) {
        var info=FileVersionInfo.GetVersionInfo(target);
        Require(Same(info.OriginalFilename,Exe) && info.ProductName=="Civ6 2K Online Fix");
        return info.ProductVersion;
    }
    static string Target(string arg) {
        string t=Path.GetFullPath(arg);
        Require(Same(t,arg) && Same(Path.GetFileName(t),Exe) && !t.StartsWith(@"\\") &&
                (File.GetAttributes(t)&FileAttributes.ReparsePoint)==0);
        for(var dir=Directory.GetParent(t);dir!=null;dir=dir.Parent)
            Require((dir.Attributes&FileAttributes.ReparsePoint)==0);
        return t;
    }
    static int Check(string target,bool manual){
        var marker=Path.Combine(Directory.GetParent(Stage.TrimEnd(Path.DirectorySeparatorChar)).FullName,"last-check.utc");
        if(!manual && File.Exists(marker)){
            var age=DateTime.UtcNow-File.GetLastWriteTimeUtc(marker);
            if(age>=TimeSpan.Zero && age<TimeSpan.FromDays(1))return 0;
        }
        var json=DownloadText(new Uri("https://api.github.com/repos/"+Repo+"/releases/latest"),true,512*1024);
        var obj=new JavaScriptSerializer().Deserialize<Dictionary<string,object>>(json);
        var tag=obj["tag_name"] as string;
        Require(obj["prerelease"] is bool && !(bool)obj["prerelease"] && obj["draft"] is bool &&
                !(bool)obj["draft"] && tag!=null && tag.StartsWith("v") && Stable(tag.Substring(1)));
        if(File.Exists(marker)){
            Require((File.GetAttributes(marker)&FileAttributes.ReparsePoint)==0);
            File.SetLastWriteTimeUtc(marker,DateTime.UtcNow);
        } else using(new FileStream(marker,FileMode.CreateNew,FileAccess.Write,FileShare.Read)){}
        if(!Newer(tag.Substring(1),ProductVersion(target))) return 0;
        File.WriteAllText(Staged("version.txt"),tag.Substring(1),Utf8);
        return 20;
    }
    internal static void Extract(string zip,string payload,Manifest m) {
        using(var fs=File.OpenRead(zip)){
            Require(fs.Length==m.ZipSize && Same(Digest(fs),m.ZipHash));fs.Position=0;
            using(var z=new ZipArchive(fs,ZipArchiveMode.Read)){
                Require(z.Entries.Count<=4096);
                var name="Civ6_2K_Online_Fix_"+m.Version+"/"+Exe;ZipArchiveEntry match=null;
                foreach(var e in z.Entries) if(Same(e.FullName.Replace('\\','/'),name)){Require(match==null);match=e;}
                Require(match!=null && match.Length==m.ExeSize);
                using(var src=match.Open())using(var to=new FileStream(payload,FileMode.CreateNew,FileAccess.Write,FileShare.None)){
                    BoundedCopy(src,to,m.ExeSize);to.Flush(true);
                }
            }
        }
        Require(new FileInfo(payload).Length==m.ExeSize && Same(HashFile(payload),m.ExeHash));
    }
    static void Prepare(string target,string version){
        Require(Newer(version,ProductVersion(target)));
        var start="https://github.com/"+Repo+"/releases/download/v"+version+"/Civ6_2K_Online_Fix_"+version;
        var envelope=DownloadText(new Uri(start+".update"),false,32768);
        var m=Verify(envelope,version,Key());File.WriteAllText(Staged("envelope.txt"),envelope,Utf8);
        using(var zip=new FileStream(Staged("bundle.zip"),FileMode.CreateNew,FileAccess.Write,FileShare.None)){
            Download(new Uri(start+".zip"),false,zip,m.ZipSize);zip.Flush(true);
        }
        Extract(Staged("bundle.zip"),Staged("payload.exe"),m);
        Require(ProductVersion(Staged("payload.exe"))==version);
        TestExe(Staged("payload.exe"));
    }
    static Process Run(string target,string args,bool hidden) {
        return Process.Start(new ProcessStartInfo(target,args){WorkingDirectory=Path.GetDirectoryName(target),
            UseShellExecute=false,CreateNoWindow=hidden,WindowStyle=hidden?ProcessWindowStyle.Hidden:ProcessWindowStyle.Normal});
    }
    static void TestExe(string exe) {
        using(var child=Run(exe,"--self-test",true)){
            Require(child!=null);
            if(!child.WaitForExit(30000)){
                child.Kill();Require(child.WaitForExit(5000));throw new TimeoutException("offline self-test timed out");
            }
            Require(child.ExitCode==0);
        }
    }
    static void RestartVerified(string exe) {
        var token=Guid.NewGuid().ToString("N");
        bool r,c;
        using(var ready=new EventWaitHandle(false,EventResetMode.ManualReset,@"Local\Civ6Fix-ready-"+token,out r))
        using(var commit=new EventWaitHandle(false,EventResetMode.ManualReset,@"Local\Civ6Fix-commit-"+token,out c)){
            Require(r && c);
            var si=new ProcessStartInfo(exe,"--watch-only"){WorkingDirectory=Path.GetDirectoryName(exe),UseShellExecute=false};
            si.EnvironmentVariables["CIV6FIX_UPDATE_HANDOFF"]=token;
            using(var child=Process.Start(si)){
                Require(child!=null);
                var watch=Stopwatch.StartNew();
                while(!ready.WaitOne(100)){
                    if(child.HasExited)throw new InvalidDataException("updated launcher exited before UI startup");
                    if(watch.Elapsed.TotalSeconds>15){
                        // Our own freshly created launcher has not started a game session.
                        child.Kill();Require(child.WaitForExit(5000));
                        throw new TimeoutException("updated launcher did not acknowledge startup");
                    }
                }
                Require(commit.Set());
            }
        }
    }
    internal static void Swap(string target,string incoming,string backup,string previousHash,string nextHash,Action validate) {
        Require(!File.Exists(backup) && Same(HashFile(target),previousHash) && Same(HashFile(incoming),nextHash));
        try {
            File.Replace(incoming,target,backup,false);
            Require(Same(HashFile(target),nextHash));validate();
        } catch {
            if(File.Exists(backup) && Same(HashFile(backup),previousHash)){
                if(!File.Exists(target))File.Move(backup,target);
                else if(Same(HashFile(target),nextHash))File.Replace(backup,target,null,false);
            }
            throw;
        }
    }
    static void Apply(string target,string version,int pid,long creation) {
        Require(new FileInfo(Staged("envelope.txt")).Length<=32768);
        var m=Verify(File.ReadAllText(Staged("envelope.txt"),Utf8),version,Key());
        Require(Newer(version,ProductVersion(target)) && new FileInfo(Staged("payload.exe")).Length==m.ExeSize &&
                Same(HashFile(Staged("payload.exe")),m.ExeHash));
        string old=HashFile(target),id=Guid.NewGuid().ToString("N");
        string next=target+"."+id+".new", backup=target+"."+id+".bak";
        bool parentExited=false,ownedIncoming=false;
        try {
            using(var parent=Process.GetProcessById(pid)){
                Require(parent.StartTime.ToUniversalTime().ToFileTimeUtc()==creation &&
                        Same(Path.GetFullPath(parent.MainModule.FileName),target));
                using(var src=File.OpenRead(Staged("payload.exe")))
                using(var dst=new FileStream(next,FileMode.CreateNew,FileAccess.Write,FileShare.None)){
                    ownedIncoming=true;BoundedCopy(src,dst,m.ExeSize);dst.Flush(true);
                }
                Require(Same(HashFile(next),m.ExeHash));
                File.WriteAllText(Staged("armed"),"ready",Utf8);
                Require(parent.WaitForExit(60000));parentExited=true;
            }
            Swap(target,next,backup,old,m.ExeHash,delegate {
                TestExe(target);
                RestartVerified(target);
            });
            // Leave a recoverable backup if removal is denied. Never touch user config or logs.
            try { File.Delete(backup); } catch(IOException) {} catch(UnauthorizedAccessException) {}
        } catch {
            if(parentExited && File.Exists(target) && Same(HashFile(target),old)){
                using(var child=Run(target,"--watch-only",false)){}
            }
            throw;
        } finally {
            if(ownedIncoming && File.Exists(next))File.Delete(next);
        }
    }
    public static int Main(string[] args){
        try {
            ServicePointManager.SecurityProtocol=SecurityProtocolType.Tls12;
            Require(args.Length>=2);
            CheckStage();
            var t=Target(args[1]);
            if(args[0]=="check" && args.Length==3)return Check(t,args[2]=="manual");
            if(args[0]=="prepare" && args.Length==3){Prepare(t,args[2]);return 0;}
            if(args[0]=="apply" && args.Length==5){Apply(t,args[2],int.Parse(args[3],CultureInfo.InvariantCulture),long.Parse(args[4],CultureInfo.InvariantCulture));return 0;}
        } catch(Exception) { return 1; }
        return 2;
    }
}
