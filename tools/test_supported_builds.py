"""Exercise exact whole-file admission with production hashing and mutations."""
import argparse,datetime,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
from supported_builds import CANONICAL_SHA256,BOOTSTRAP_SHA256,SUPPORTED_SHA256,checked_file_hash,is_supported_hash,POLICY_HEADER
PROJECT=Path(__file__).resolve().parents[1];ROOT=PROJECT.parent

def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--canonical',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=PROJECT/'verification/permanent-os-fix/identity-tests')
    parser.add_argument('--compiler',type=Path,default=Path('C:/msys64/mingw64/bin/g++.exe'))
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    manifest=json.loads(args.manifest.read_text(encoding='utf-8-sig'))
    assert (manifest['original_sha256'],manifest['patched_sha256'])==SUPPORTED_SHA256
    assert sha(args.canonical)==CANONICAL_SHA256 and sha(args.candidate)==BOOTSTRAP_SHA256
    fixture=PROJECT/'tools/supported_builds_fixture.cpp';executable=args.output/'supported_builds_fixture.exe'
    inputs=[POLICY_HEADER,PROJECT/'src/debug_settings.cpp',PROJECT/'src/debug_settings.h',PROJECT/'src/debug_settings_bindings.inc',
        PROJECT/'src/engine_dispatch.h',fixture,Path(__file__).resolve(),PROJECT/'tools/supported_builds.py',
        PROJECT/'tools/supported_builds.ps1',args.manifest,args.canonical,args.candidate]
    before={str(path):sha(path) for path in inputs}
    command=[str(args.compiler),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX','-municode',
        '-static','-static-libgcc','-static-libstdc++',str(fixture),'-o',str(executable),'-lbcrypt']
    environment=dict(os.environ);environment['PATH']=str(args.compiler.parent)+';'+environment.get('PATH','')
    subprocess.run(command,check=True,env=environment,capture_output=True,text=True)
    cases=[]
    def check_file(name,path,expected):
        actual=sha(path);compiled=json.loads(subprocess.check_output([str(executable),str(path)],env=environment,text=True))
        assert compiled['sha256']==actual and compiled['supported'] is expected
        assert is_supported_hash(actual) is expected
        try:result=checked_file_hash(path)
        except ValueError:assert not expected
        else:assert expected and result==actual
        # Use the actual PowerShell admission helper with a literal path supplied
        # in a child environment variable; no shell interpolation of filenames.
        psEnv=dict(environment);psEnv['SM2_IDENTITY_TEST_INPUT']=str(path.resolve());psEnv['SM2_IDENTITY_TEST_POLICY']=str((PROJECT/'tools/supported_builds.ps1').resolve())
        ps=". $env:SM2_IDENTITY_TEST_POLICY; try { $digest = Get-SM2SupportedGameHash $env:SM2_IDENTITY_TEST_INPUT; @{supported=$true;sha256=$digest} | ConvertTo-Json -Compress } catch { @{supported=$false} | ConvertTo-Json -Compress }"
        powershell=json.loads(subprocess.check_output(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-Command',ps],env=psEnv,text=True))
        assert powershell['supported'] is expected
        if expected:assert powershell['sha256']==actual
        cases.append({'name':name,'sha256':actual,'expected_admission':expected,'cpp':compiled,'python_agrees':True,'powershell':powershell,'passed':True})
    check_file('canonical original accepted',args.canonical,True)
    check_file('exact approved bootstrap accepted',args.candidate,True)
    # A single owned scratch file is overwritten between negative cases; neither
    # source EXE changes. Keep only the manifest evidence, not 192MB test copies.
    scratch=args.output/'identity-mutation.bin'
    try:
        for name,source,kind in [('canonical unrelated one-byte mutation',args.canonical,'byte'),
                ('bootstrap unrelated one-byte mutation',args.candidate,'byte'),
                ('bootstrap partial rollback',args.candidate,'partial'),
                ('bootstrap appended byte',args.candidate,'append')]:
            shutil.copyfile(source,scratch)
            with scratch.open('r+b') as target:
                if kind=='byte':
                    target.seek(0x1000);value=target.read(1);assert value;target.seek(0x1000);target.write(bytes([value[0]^1]))
                elif kind=='partial':
                    span=next(item for item in manifest['patch_spans'] if item['reason']=='entrypoint')
                    target.seek(span['offset']);target.write(bytes.fromhex(span['original_hex']))
                else:target.seek(0,2);target.write(b'\0')
            check_file(name,scratch,False)
    finally:
        if scratch.is_file():scratch.unlink()
    for value in ['',CANONICAL_SHA256.upper(),BOOTSTRAP_SHA256+'0','0'*64]:assert not is_supported_hash(value)
    for path,expected in before.items():assert sha(path)==expected,'Input changed during identity fixture: '+path
    report={'schema':1,'generated_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'passed':True,
        'input_sha256':before,'compiler':str(args.compiler),'compile_arguments':command,'executable_sha256':sha(executable),
        'cases':cases,'case_count':len(cases),'malformed_digest_refusals':4,
        'game_started':False,'native_game_calls':0,'installed_files_changed':False,
        'scope':'Compiled production HashFile plus exact shared admission helper, Python and PowerShell admission agree on canonical/candidate and full-file mutations.'}
    (args.output/'supported-build-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':True,'cases':len(cases),'malformed_digest_refusals':4,'report':str(args.output/'supported-build-tests.json')}))

if __name__=='__main__':main()
