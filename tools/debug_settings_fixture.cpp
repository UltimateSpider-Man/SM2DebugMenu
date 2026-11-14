// Current-process synthetic metadata/scalars only. Never loads the game.
#include "../src/debug_settings.cpp"
#include <iostream>
#include <stdexcept>
#include <condition_variable>
#include <chrono>
#include <thread>

bool fixtureContext = false;
unsigned contextChecks = 0;
namespace sm2debug {
bool IsWithinVerifiedEngineCallback() noexcept { ++contextChecks; return fixtureContext; }
}
namespace {
using namespace sm2debug;
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
struct Fixture {
    unsigned char* bytes = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Image image;
    std::vector<BindingSpec> specs;
    Fixture() {
        Check(bytes != nullptr, "Cannot allocate synthetic image");
        image.base = reinterpret_cast<std::uintptr_t>(bytes); image.size = 0x10000;
        image.writable.push_back({image.base + 0x8000, 0x2000});
        Add("Bool", DebugSettingType::Boolean, 15, 0);
        Add("Int", DebugSettingType::Int32, 6, 12);
        Add("UInt", DebugSettingType::UInt32, 2, 1024);
        Add("Float", DebugSettingType::Float32, 8, FloatBits(0.002f));
        Add("Enum", DebugSettingType::Enum32, 11, 1);
        Add("Min", DebugSettingType::Float32, 8, FloatBits(0.002f));
        Add("Max", DebugSettingType::Float32, 8, FloatBits(0.014f));
        specs[5].peer = "Test.Max"; specs[5].relation = -1;
        specs[6].peer = "Test.Min"; specs[6].relation = 1;
        InitializeEntries(image, specs);
    }
    ~Fixture() { settings.entries.clear();settings.rawBooleans.clear(); settings.initialized = false; VirtualFree(bytes, 0, MEM_RELEASE); }
    static std::uint32_t FloatBits(float number) { std::uint32_t bits; std::memcpy(&bits, &number, 4); return bits; }
    template<class T> void Put(std::size_t offset, T value) {
        Check(offset <= 0x10000 && sizeof(T) <= 0x10000 - offset, "Fixture write overflow");
        std::memcpy(bytes + offset, &value, sizeof(value));
    }
    void Text(std::size_t offset, const std::string& value) {
        Check(offset <= 0x10000 && value.size()+1 <= 0x10000-offset, "Fixture string overflow");
        std::memcpy(bytes+offset,value.c_str(),value.size()+1);
    }
    std::uint32_t Bits(unsigned index) const { std::uint32_t result=0;std::memcpy(&result,bytes+specs[index].live,Width(specs[index]));return result; }
    void Add(const std::string& name, DebugSettingType kind, unsigned code, std::uint32_t bits) {
        const std::size_t index = specs.size(), r = 0x1000 + index*0x800;
        BindingSpec spec{};
        spec.id="Test."+name;spec.page="fixture";spec.label=name;spec.group="Test";spec.field=name;spec.descriptorName="TestSettings";
        spec.registration=r;spec.descriptor=r+0x40;spec.live=0x8000+index*8;spec.defaults=0x9000+index*8;
        spec.objectSize=4;spec.fieldCount=1;spec.index=spec.offset=0;spec.code=code;spec.type=kind;
        spec.step=kind==DebugSettingType::Float32?0.0001:1;spec.minimum=0;
        spec.maximum=kind==DebugSettingType::Float32?std::numeric_limits<float>::max():kind==DebugSettingType::UInt32?4294967295.0:2147483647.0;
        Put(r,image.base+r+0x180);Put(r+8,image.base+spec.live);Put(r+16,image.base+spec.descriptor);
        Text(r+0x180,"Test");Text(r+0x1c0,"TestSettings");Text(r+0x200,name);
        const auto d=spec.descriptor;
        Put(d,image.base+r+0x1c0);Put(d+0x14,std::uint32_t(4));Put(d+0x18,std::uint16_t(1));Put(d+0x50,image.base+spec.defaults);
        Put(d+0x60,image.base+r+0x240);Put(r+0x240,image.base+r+0x200);
        Put(d+0x68,image.base+r+0x250);Put(r+0x250,static_cast<unsigned char>(code));
        Put(d+0x80,image.base+r+0x260);Put(r+0x260,std::uint32_t(1));
        Put(d+0x88,image.base+r+0x270);Put(r+0x270,std::uint32_t(0));
        Put(d+0x90,image.base+r+0x280);
        if(kind==DebugSettingType::Enum32){
            spec.enumDescriptor=r+0x300;spec.enumType=0xaabbccdd;
            spec.choices={{0,"kZero","Zero"},{1,"kOne","One"},{2,"kTwo","Two"}};
            spec.maximum=2; // The reviewed unrestricted range covers all three native ordinals.
            Put(r+0x280,spec.enumType);Put(r+0x308,spec.enumType);Put(r+0x30c,std::uint32_t(3));
            Put(r+0x328,image.base+r+0x400);Put(r+0x338,image.base+r+0x420);
            for(std::size_t i=0;i<spec.choices.size();++i){
                const auto n=r+0x450+i*0x40;
                Text(n,spec.choices[i].name);Text(n+0x20,spec.choices[i].label);
                Put(r+0x400+i*8,image.base+n);Put(r+0x420+i*8,image.base+n+0x20);
            }
        }
        Put(spec.live,bits);Put(spec.defaults,std::uint32_t(0x11111111));specs.push_back(spec);
    }
    DebugSettingValue Read(unsigned i) { return ReadDebugSetting("fixture",specs[i].label); }
    bool Apply(unsigned i,int direction=1) { return ApplyDebugSetting(specs[i].id,direction); }
};
void Exercise() {
    Check(!InitializeDebugSettings(GetModuleHandleW(nullptr)),"Wrong process initialized retail bindings");
    Check(SnapshotDebugSettings().empty(),"Wrong process exposed values");
    Fixture f;
    const auto noContextReads=contextChecks;
    auto snapshot=SnapshotDebugSettings();
    Check(snapshot.size()==7,"Snapshot omitted allowlisted fixture values");
    for(const auto& value:snapshot)Check(value.available&&value.pageId=="fixture"&&!value.label.empty(),"Read-only snapshot metadata unavailable");
    Check(contextChecks==noContextReads,"Snapshot tried to enter engine context");
    Check(f.Read(1).number==12&&f.Read(2).number==1024,"Integer decoding failed");
    Check(std::fabs(f.Read(3).number-0.002)<0.000001,"Float decoding failed");
    Check(f.Read(4).choices==std::vector<std::string>{"Zero","One","Two"},"Enum table decoding failed");
    std::array<std::uint32_t,7> initial{};for(unsigned i=0;i<7;++i)initial[i]=f.Bits(i);
    for(unsigned i=0;i<7;++i)Check(!f.Apply(i),"Unregistered context wrote a scalar");
    RestoreDebugSettings();for(unsigned i=0;i<7;++i)Check(f.Bits(i)==initial[i],"Context refusal changed memory");
    fixtureContext=true;
    auto& spec=f.specs[0];
    f.Put(spec.registration+8,f.image.base+spec.defaults);
    Check(!f.Read(0).available&&!f.Apply(0),"Default object accepted as live object");
    f.Put(spec.registration+8,f.image.base+spec.live);
    Check(f.Bits(0)==0,"Default-alias rejection wrote live memory");
    f.Put(spec.registration+0x260,std::uint32_t(2));Check(!f.Read(0).available,"Array accepted as scalar");
    f.Put(spec.registration+0x260,std::uint32_t(1));
    f.Put(spec.registration+0x270,std::uint32_t(4));Check(!f.Read(0).available,"Out-of-object offset accepted");
    f.Put(spec.registration+0x270,std::uint32_t(0));
    f.Put(spec.descriptor+0x18,std::uint16_t(0));Check(!f.Read(0).available,"Zero-field descriptor accepted");
    f.Put(spec.descriptor+0x18,std::uint16_t(1));
    f.Put(spec.registration+0x250,static_cast<unsigned char>(8));Check(!f.Read(0).available,"Metadata type mismatch accepted");
    f.Put(spec.registration+0x250,static_cast<unsigned char>(15));
    f.Text(spec.registration+0x200,"Other");Check(!f.Read(0).available,"Unexpected field name accepted");f.Text(spec.registration+0x200,"Bool");
    DWORD oldProtection=0,ignored=0;
    Check(VirtualProtect(f.bytes+0x8000,4096,PAGE_READONLY,&oldProtection)!=FALSE,"Cannot protect fixture page");
    const bool readOnlyRefused=!f.Read(0).available&&!f.Apply(0);
    VirtualProtect(f.bytes+0x8000,4096,oldProtection,&ignored);Check(readOnlyRefused,"Read-only live page accepted");
    f.Put(f.specs[4].live,std::uint32_t(3));Check(!f.Read(4).available&&!f.Apply(4),"Invalid enum ordinal accepted");f.Put(f.specs[4].live,std::uint32_t(1));
    f.Put(f.specs[4].enumDescriptor+12,std::uint32_t(4));Check(!f.Read(4).available,"Changed enum table accepted");f.Put(f.specs[4].enumDescriptor+12,std::uint32_t(3));
    f.Put(f.specs[3].live,Fixture::FloatBits(std::numeric_limits<float>::quiet_NaN()));Check(!f.Read(3).available&&!f.Apply(3),"NaN accepted");f.Put(f.specs[3].live,initial[3]);
    f.Put(f.specs[2].live,std::numeric_limits<std::uint32_t>::max());Check(!f.Apply(2),"UInt32 increment overflow accepted");f.Put(f.specs[2].live,initial[2]);
    f.Put(f.specs[1].live,std::uint32_t(0x7fffffff));Check(!f.Apply(1),"Int32 increment overflow accepted");f.Put(f.specs[1].live,initial[1]);
    Check(f.Apply(0,0)&&f.Bits(0)==1,"Verified-scope bool toggle failed");
    fixtureContext=false;Check(!RestoreDebugSettings(),"Unregistered context falsely acknowledged restore");Check(f.Bits(0)==1,"Restore wrote outside verified callback");
    fixtureContext=true;Check(RestoreDebugSettings(),"Owned bool restore was not acknowledged");Check(f.Bits(0)==0,"Owned boolean was not restored");
    Check(f.Apply(1)&&f.Bits(1)==13,"Int32 increment failed");f.Put(f.specs[1].live,std::uint32_t(99));RestoreDebugSettings();Check(f.Bits(1)==99,"Restore overwrote later external edit");
    Check(f.Apply(1)&&f.Bits(1)==100,"Explicit edit did not start at external value");RestoreDebugSettings();Check(f.Bits(1)==99,"Restore lost new external baseline");
    Check(f.Apply(4,0)&&f.Read(4).number==2,"Enum forward cycle failed");Check(f.Apply(4,0)&&f.Read(4).number==0,"Enum wrap failed");RestoreDebugSettings();Check(f.Read(4).number==1,"Enum original not restored");
    Check(f.Apply(4,-1)&&f.Read(4).number==0,"Enum reverse failed");RestoreDebugSettings();
    Check(f.Apply(3)&&f.Read(3).number>0.002,"Float scalar increment failed");RestoreDebugSettings();Check(f.Bits(3)==initial[3],"Float rollback changed bits");
    settings.entries[5].spec.step=0.02;Check(!f.Apply(5),"Minimum budget exceeded paired maximum");
    settings.entries[6].spec.step=0.02;Check(!f.Apply(6,-1),"Maximum budget fell below paired minimum");
    std::uintptr_t address=0;Check(Validate(f.image,f.specs[1],address),"Valid integer address rejected");
    Check(!CompareExchange(f.image,f.specs[1],address,12,1)&&f.Bits(1)==99,"Compare/exchange clobbered changed value");
    Check(!ApplyDebugSetting("Unknown",1)&&!f.Apply(0,2),"Unknown ID/direction accepted");
    for(std::size_t i=0;i<3;++i){
        settings.entries[4].spec.choices[i].label="NONE";
        f.Text(f.specs[4].registration+0x450+i*0x40+0x20,"NONE");
    }
    Check(f.Read(4).choices==std::vector<std::string>{"kZero","kOne","kTwo"},"Duplicate enum display labels were not distinguished by exact member names");
    Check(f.Apply(0,0)&&f.Bits(0)==1,"Cannot seed owned value for contention test");
    std::mutex gate;std::condition_variable changed;bool held=false,released=false;
    std::thread holder([&]{
        std::unique_lock guard(settings.mutex);
        {std::lock_guard mark(gate);held=true;}changed.notify_all();
        std::unique_lock wait(gate);changed.wait_for(wait,std::chrono::seconds(1),[&]{return released;});
    });
    {std::unique_lock wait(gate);changed.wait(wait,[&]{return held;});}
    const auto before=GetTickCount64();
    const bool contendedApply=f.Apply(0,0);const bool contendedRestore=RestoreDebugSettings();
    const auto elapsed=GetTickCount64()-before;
    {std::lock_guard release(gate);released=true;}changed.notify_all();holder.join();
    Check(!contendedApply&&!contendedRestore&&elapsed<200,"Engine callback blocked/acknowledged contended worker snapshot mutex");
    Check(f.Bits(0)==1&&settings.entries[0].owned,"Contended restore lost pending ownership");
    Check(RestoreDebugSettings(),"Deferred restore was not acknowledged on retry");Check(f.Bits(0)==0,"Deferred owned restore did not succeed on retry");
    f.Put(f.specs[5].live,Fixture::FloatBits(0.01f));f.Put(f.specs[6].live,Fixture::FloatBits(0.02f));
    settings.entries[5].spec.step=0.009;settings.entries[6].spec.step=0.015;
    Check(f.Apply(5,-1)&&f.Apply(6,-1),"Cannot seed lowered paired budgets");
    Check(!RestoreDebugSettings(),"Peer-invalid first restore falsely acknowledged completion");
    Check(f.Read(5).number<f.Read(6).number&&f.Bits(5)!=Fixture::FloatBits(0.01f)&&f.Bits(6)==Fixture::FloatBits(0.02f),
        "First paired rollback violated order or failed to restore maximum");
    Check(RestoreDebugSettings()&&f.Bits(5)==Fixture::FloatBits(0.01f)&&f.Bits(6)==Fixture::FloatBits(0.02f),
        "Second paired rollback did not restore exact original bits");
    for(unsigned i=0;i<7;++i)Check(*reinterpret_cast<std::uint32_t*>(f.bytes+f.specs[i].defaults)==0x11111111,"Default instance was modified");
    fixtureContext=false;
}
void RestrictedEnumPolicy() {
    Fixture f;
    fixtureContext=true;
    constexpr unsigned index=4;
    auto& entry=settings.entries[index];
    auto& spec=entry.spec;
    // Reproduce the native Weapon enum domain independently of the generated
    // production allowlist: all three ordinals remain valid for reading and
    // rollback, while user selection is explicitly limited to Off/On.
    const std::array<const char*,3> nativeNames{{"Off","On","NoReload"}};
    for(std::size_t i=0;i<nativeNames.size();++i){
        spec.choices[i].name=nativeNames[i];spec.choices[i].label="NONE";
        f.Text(f.specs[index].registration+0x450+i*0x40,nativeNames[i]);
        f.Text(f.specs[index].registration+0x450+i*0x40+0x20,"NONE");
    }
    spec.minimum=0;spec.maximum=1;
    Check(f.Read(index).available&&f.Read(index).choices==std::vector<std::string>{"Off","On","NoReload"},
          "Restricted write range hid a native enum member from the read snapshot");
    for(std::uint32_t start : {0u,1u})for(int direction : {-1,0,1}){
        f.Put(f.specs[index].live,start);
        Check(!entry.owned,"Prior enum case retained ownership");
        const std::uint32_t expected=start==0?1:0;
        Check(f.Apply(index,direction)&&f.Bits(index)==expected,
              "Off/On selector failed to wrap for left/right/accept");
        Check(f.Bits(index)!=2,"Restricted selector produced unreviewed NoReload");
        Check(RestoreDebugSettings()&&f.Bits(index)==start&&!entry.owned,
              "Restricted enum did not restore its exact original ordinal");
    }
    for(int direction : {-1,0,1}){
        f.Put(f.specs[index].live,std::uint32_t(2));
        Check(f.Read(index).available&&f.Read(index).number==2,
              "An external native NoReload value became unreadable");
        const std::uint32_t expected=direction<0?1:0;
        Check(f.Apply(index,direction)&&f.Bits(index)==expected,
              "External NoReload did not enter the reviewed range at the appropriate end");
        Check(entry.owned&&entry.original==2,"Restricted edit lost the external NoReload baseline");
        fixtureContext=false;
        Check(!RestoreDebugSettings()&&f.Bits(index)==expected&&entry.owned,
              "Restricted enum restore wrote or lost ownership outside engine context");
        fixtureContext=true;
        Check(RestoreDebugSettings()&&f.Bits(index)==2&&!entry.owned,
              "Owned restricted edit did not restore exact native NoReload");
    }
    f.Put(f.specs[index].live,std::uint32_t(2));
    Check(f.Apply(index,1)&&f.Bits(index)==0,"Cannot seed restricted external-edit case");
    f.Put(f.specs[index].live,std::uint32_t(1));
    Check(RestoreDebugSettings()&&f.Bits(index)==1&&!entry.owned,
          "Restricted rollback overwrote a later external On edit");
    Check(f.Apply(index,1)&&f.Bits(index)==0,"Restricted selector failed after external ownership change");
    Check(RestoreDebugSettings()&&f.Bits(index)==1,"Restricted edit failed to adopt the new external baseline");
    f.Put(f.specs[index].live,std::uint32_t(0));
    Check(f.Apply(index,1)&&f.Bits(index)==1,"Cannot seed external NoReload edit");
    f.Put(f.specs[index].live,std::uint32_t(2));
    Check(RestoreDebugSettings()&&f.Bits(index)==2&&!entry.owned,
          "Restricted rollback overwrote later external NoReload");

    const double nan=std::numeric_limits<double>::quiet_NaN();
    const double infinity=std::numeric_limits<double>::infinity();
    const std::array<std::pair<double,double>,13> invalid{{
        {nan,1},{0,nan},{infinity,1},{0,infinity},
        {-1,1},{0,-1},{0.5,1},{0,1.5},{2,1},
        {3,3},{0,3},{0,2147483647.0},{-0.5,1.5}
    }};
    for(const auto& range:invalid){
        spec.minimum=range.first;spec.maximum=range.second;
        f.Put(f.specs[index].live,std::uint32_t(1));
        Check(f.Read(index).available&&f.Read(index).number==1,
              "Malformed selection policy changed validation of a native readable value");
        for(int direction:{-1,0,1}){
            Check(!f.Apply(index,direction)&&f.Bits(index)==1&&!entry.owned,
                  "Invalid enum selection bounds reached a write or ownership change");
        }
    }
    spec.minimum=0;spec.maximum=1;
    for(std::uint32_t invalidOrdinal:{3u,std::numeric_limits<std::uint32_t>::max()}){
        f.Put(f.specs[index].live,invalidOrdinal);
        Check(!f.Read(index).available,"Out-of-domain native ordinal was readable");
        for(int direction:{-1,0,1})Check(!f.Apply(index,direction)&&f.Bits(index)==invalidOrdinal,
              "Out-of-domain native ordinal was silently replaced");
    }
    // A reviewed range with a nonzero lower bound also needs modular indexing
    // relative to that bound; it must not accidentally select ordinal zero.
    spec.minimum=1;spec.maximum=2;
    f.Put(f.specs[index].live,std::uint32_t(1));
    Check(f.Apply(index,-1)&&f.Bits(index)==2,"Nonzero enum lower bound failed reverse wrap");
    Check(f.Apply(index,1)&&f.Bits(index)==1,"Nonzero enum lower bound failed forward wrap");
    Check(RestoreDebugSettings()&&f.Bits(index)==1,"Nonzero enum lower bound failed restore");
    spec.minimum=0;spec.maximum=2;
    f.Put(f.specs[index].live,std::uint32_t(2));
    Check(f.Apply(index,0)&&f.Bits(index)==0,"Unrestricted native three-choice cycle failed");
    Check(f.Apply(index,-1)&&f.Bits(index)==2,"Unrestricted native three-choice reverse wrap failed");
    Check(RestoreDebugSettings()&&f.Bits(index)==2,"Unrestricted native three-choice restore failed");
    Check(*reinterpret_cast<std::uint32_t*>(f.bytes+f.specs[index].defaults)==0x11111111,
          "Restricted selection policy modified the DDL default object");
    fixtureContext=false;
}
unsigned nestedRejectionCases = 0;
struct NestedFixture : Fixture {
    static constexpr unsigned index = 7;
    static constexpr std::size_t registration=0x5000, parentDescriptor=0x5100, childDescriptor=0x5800;
    static constexpr std::size_t parentLive=0x8800, parentDefaults=0x9800, childLive=0x8820, childDefaults=0x9900;
    static constexpr std::uint32_t childTypeId=0x112233aa;
    NestedFixture() {
        // These eight regions are independent of the seven legacy fixture
        // objects: registration, two descriptors/arrays, two names regions,
        // parent live storage, parent defaults and child defaults. The child
        // itself is embedded in the parent's live object, exactly as DDL13.
        BindingSpec spec{};
        spec.id="TrafficFixture.PedestrianLoader.Enabled";spec.page="nested-fixture";spec.label="Enable Pedestrians";
        spec.group="TrafficFixture";spec.field="Enabled";spec.descriptorName="PedestrianFixtureSettings";
        spec.registration=registration;spec.descriptor=childDescriptor;spec.live=childLive;spec.defaults=childDefaults;
        spec.objectSize=0x10;spec.fieldCount=3;spec.index=1;spec.offset=4;spec.code=15;spec.type=DebugSettingType::Boolean;
        spec.step=1;spec.minimum=0;spec.maximum=1;
        spec.parent={"PedestrianLoader","TrafficFixtureSettings",parentLive,parentDescriptor,parentDefaults,0x80,3,1,0x20,childTypeId};
        Put(registration,image.base+0x5280);Put(registration+8,image.base+parentLive);Put(registration+16,image.base+parentDescriptor);
        Text(0x5280,spec.group);Text(0x5300,spec.parent.descriptorName);
        Text(0x5400,"Before");Text(0x5440,spec.parent.field);Text(0x5480,"After");
        Put(parentDescriptor,image.base+0x5300);Put(parentDescriptor+0x14,std::uint32_t(0x80));
        Put(parentDescriptor+0x18,std::uint16_t(3));Put(parentDescriptor+0x50,image.base+parentDefaults);
        Put(parentDescriptor+0x60,image.base+0x5380);Put(parentDescriptor+0x68,image.base+0x53a0);
        Put(parentDescriptor+0x80,image.base+0x53b0);Put(parentDescriptor+0x88,image.base+0x53c0);Put(parentDescriptor+0x90,image.base+0x53d0);
        Text(0x5a00,spec.descriptorName);Text(0x5a40,"Before");Text(0x5a60,spec.field);Text(0x5a80,"After");
        Put(childDescriptor,image.base+0x5a00);Put(childDescriptor+8,childTypeId);
        Put(childDescriptor+0x14,std::uint32_t(0x10));Put(childDescriptor+0x18,std::uint16_t(3));Put(childDescriptor+0x50,image.base+childDefaults);
        Put(childDescriptor+0x60,image.base+0x5b00);Put(childDescriptor+0x68,image.base+0x5b30);
        Put(childDescriptor+0x80,image.base+0x5b40);Put(childDescriptor+0x88,image.base+0x5b50);Put(childDescriptor+0x90,image.base+0x5b60);
        for(std::size_t i=0;i<3;++i){
            Put(0x5380+i*8,image.base+0x5400+i*0x40);Put(0x53a0+i,static_cast<unsigned char>(i==1?13:2));
            Put(0x53b0+i*4,std::uint32_t(1));Put(0x53c0+i*4,std::uint32_t(i==1?0x20:i==0?0:0x60));
            Put(0x53d0+i*4,std::uint32_t(i==1?childTypeId:0));
            Put(0x5b00+i*8,image.base+0x5a40+i*0x20);Put(0x5b30+i,static_cast<unsigned char>(i==1?15:2));
            Put(0x5b40+i*4,std::uint32_t(1));Put(0x5b50+i*4,std::uint32_t(i==1?4:i==0?0:8));Put(0x5b60+i*4,std::uint32_t(0));
        }
        std::memset(bytes+parentLive,0xa7,0x80);std::memset(bytes+parentDefaults,0xcd,0x80);std::memset(bytes+childDefaults,0xe3,0x10);
        Put(childLive,std::uint32_t(0x11223344));Put(childLive+4,static_cast<unsigned char>(0));Put(childLive+8,std::uint32_t(0x55667788));
        specs.push_back(spec);InitializeEntries(image,specs);
    }
    DebugSettingValue Value() { return ReadDebugSetting("nested-fixture","Enable Pedestrians"); }
    bool ApplyNested(int direction=0) { return ApplyDebugSetting(specs[index].id,direction); }
    unsigned char Leaf() const { return bytes[childLive+4]; }
    std::vector<unsigned char> Memory() const { return {bytes,bytes+0x10000}; }
    void UnchangedExceptLeaf(const std::vector<unsigned char>& before,const char* message) {
        auto after=Memory();after[childLive+4]=before[childLive+4];Check(after==before,message);
    }
    void Reject(const char* message) {
        const auto before=Memory();
        Check(!Value().available,message);
        for(int direction:{-1,0,1})Check(!ApplyNested(direction),message);
        Check(Memory()==before,"Rejected nested identity changed synthetic storage");
        Check(!settings.entries[index].owned,"Rejected nested identity acquired rollback ownership");
        ++nestedRejectionCases;
    }
    template<class T> void Corrupt(std::size_t offset,T replacement,const char* message) {
        T original{};std::memcpy(&original,bytes+offset,sizeof(original));Put(offset,replacement);
        Reject(message);Put(offset,original);Check(Value().available,"Restored nested metadata remained unavailable");
    }
    template<class Mutator> void CorruptSpec(Mutator change,const char* message) {
        auto& entry=settings.entries[index];const auto original=entry.spec;change(entry.spec);Reject(message);entry.spec=original;
        Check(Value().available,"Restored nested specification remained unavailable");
    }
};
void NestedParentPolicy() {
    NestedFixture f;fixtureContext=true;
    auto& entry=settings.entries[NestedFixture::index];
    Check(f.Value().available&&f.Value().number==0,"Valid nested DDL object was not readable");
    const auto readContextChecks=contextChecks;
    Check(SnapshotDebugSettings().size()==8,"Nested object omitted from read-only snapshot");
    Check(contextChecks==readContextChecks,"Nested snapshot attempted engine authorization");
    std::uintptr_t address=0;
    Check(Validate(f.image,entry.spec,address)&&address==f.image.base+NestedFixture::childLive+4,"Nested leaf did not resolve its nonzero child offset");
    const auto initial=f.Memory();
    fixtureContext=false;
    for(int direction:{-1,0,1})Check(!f.ApplyNested(direction),"Nested write accepted outside verified context");
    Check(f.Memory()==initial&&!entry.owned,"Unauthorized nested write changed storage/ownership");
    fixtureContext=true;
    Check(f.ApplyNested()&&f.Leaf()==1&&entry.owned&&entry.original==0,"Nested Boolean toggle failed");
    f.UnchangedExceptLeaf(initial,"Nested byte toggle changed neighbors or defaults");
    Check(!CompareExchange(f.image,entry.spec,address,0,0)&&f.Leaf()==1,"Nested CAS overwrote a changed byte");
    fixtureContext=false;
    Check(!RestoreDebugSettings()&&f.Leaf()==1&&entry.owned,"Nested restore outside context wrote or discarded ownership");
    fixtureContext=true;
    Check(RestoreDebugSettings()&&f.Memory()==initial&&!entry.owned,"Nested restore did not restore exact original storage");

    f.Corrupt(NestedFixture::registration,f.image.base+0x5400,"Wrong parent registration name accepted");
    f.Corrupt(NestedFixture::registration+8,f.image.base+NestedFixture::parentDefaults,"Parent defaults registered as live accepted");
    f.Corrupt(NestedFixture::registration+8,f.image.base+NestedFixture::childLive,"Embedded child registered instead of parent accepted");
    f.Corrupt(NestedFixture::registration+16,f.image.base+NestedFixture::childDescriptor,"Child descriptor registered instead of parent accepted");
    f.Corrupt(NestedFixture::registration+16,f.image.base+NestedFixture::parentDefaults,"Parent defaults registered as descriptor accepted");
    f.Corrupt(NestedFixture::parentDescriptor,f.image.base+0x5a00,"Wrong parent descriptor name accepted");
    f.Corrupt(NestedFixture::parentDescriptor+0x14,std::uint32_t(0x81),"Changed parent object size accepted");
    f.Corrupt(NestedFixture::parentDescriptor+0x18,std::uint16_t(2),"Changed parent field count accepted");
    f.Corrupt(NestedFixture::parentDescriptor+0x50,f.image.base+NestedFixture::parentLive,"Parent descriptor default aliases live accepted");
    f.Corrupt(NestedFixture::parentDescriptor+0x50,f.image.base+NestedFixture::childDefaults,"Wrong parent defaults accepted");
    f.Corrupt(0x5388,f.image.base+0x5400,"Wrong nested parent field name accepted");
    f.Corrupt(0x53a1,static_cast<unsigned char>(15),"Non-struct parent field type accepted");
    f.Corrupt(0x53b4,std::uint32_t(2),"Nested parent field array accepted");
    f.Corrupt(0x53c4,std::uint32_t(0x21),"Changed embedded child offset accepted");
    f.Corrupt(0x53d4,std::uint32_t(NestedFixture::childTypeId+1),"Wrong nested parent type ID accepted");
    f.Corrupt(NestedFixture::childDescriptor+8,std::uint32_t(NestedFixture::childTypeId+1),"Child descriptor type ID mismatch accepted");
    for(std::size_t member:{0x60u,0x68u,0x80u,0x88u,0x90u})
        f.Corrupt(NestedFixture::parentDescriptor+member,f.image.base+0x10000,"Out-of-image parent metadata array accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.objectSize=0;},"Zero parent size accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.fieldCount=0;},"Zero parent count accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.fieldCount=4097;},"Unbounded parent count accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.index=s.parent.fieldCount;},"Out-of-range parent field index accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.offset=s.parent.objectSize;},"Parent offset outside parent object accepted");
    f.CorruptSpec([](BindingSpec& s){s.objectSize=s.parent.objectSize-s.parent.offset+1;},"Child extent beyond parent accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.defaults=s.parent.live;},"Parent specification defaults alias live accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.typeId=0;},"Zero child structure type ID accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.live=std::numeric_limits<std::uintptr_t>::max()-0x10;},"Overflowing parent plus child offset accepted");
    f.CorruptSpec([](BindingSpec& s){++s.live;},"Child live address disconnected from parent offset accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.live=0xfff0;s.live=0x10010;},"Out-of-image parent live range accepted");
    f.CorruptSpec([](BindingSpec& s){s.parent.descriptor=0xfff0;},"Truncated parent descriptor accepted");

    f.Corrupt(NestedFixture::childDescriptor,f.image.base+0x5300,"Wrong leaf descriptor name accepted");
    f.Corrupt(NestedFixture::childDescriptor+0x14,std::uint32_t(0x11),"Changed child object size accepted");
    f.Corrupt(NestedFixture::childDescriptor+0x18,std::uint16_t(2),"Changed child field count accepted");
    f.Corrupt(NestedFixture::childDescriptor+0x50,f.image.base+NestedFixture::childLive,"Child defaults alias live accepted");
    f.Corrupt(0x5b08,f.image.base+0x5a40,"Wrong nested leaf name accepted");
    f.Corrupt(0x5b31,static_cast<unsigned char>(8),"Wrong nested leaf primitive type accepted");
    f.Corrupt(0x5b44,std::uint32_t(2),"Nested leaf array accepted as scalar");
    f.Corrupt(0x5b54,std::uint32_t(5),"Changed nested leaf offset accepted");
    f.Corrupt(NestedFixture::childLive+4,static_cast<unsigned char>(2),"Malformed nested Boolean accepted");
    for(std::size_t member:{0x60u,0x68u,0x80u,0x88u})
        f.Corrupt(NestedFixture::childDescriptor+member,f.image.base+0x10000,"Out-of-image child metadata array accepted");
    f.CorruptSpec([](BindingSpec& s){s.index=s.fieldCount;},"Out-of-range leaf index accepted");
    f.CorruptSpec([](BindingSpec& s){s.offset=s.objectSize;},"Out-of-range leaf offset accepted");
    f.CorruptSpec([](BindingSpec& s){s.defaults=s.live;},"Child specification defaults alias live accepted");
    f.CorruptSpec([](BindingSpec& s){s.descriptor=0xfff0;},"Truncated child descriptor accepted");
    Check(f.Memory()==initial,"Nested negative matrix failed to restore fixture metadata/storage");

    Check(f.ApplyNested()&&f.Leaf()==1,"Cannot seed nested deferred-identity restore");
    f.Put(0x53d4,std::uint32_t(NestedFixture::childTypeId+1));
    Check(!RestoreDebugSettings()&&entry.owned&&f.Leaf()==1,"Invalid parent identity did not defer owned restore");
    f.Put(0x53d4,NestedFixture::childTypeId);
    Check(RestoreDebugSettings()&&!entry.owned&&f.Memory()==initial,"Restored parent identity did not allow deferred rollback");
    Check(f.ApplyNested()&&f.Leaf()==1,"Cannot seed nested external-edit case");
    f.Put(NestedFixture::childLive+4,static_cast<unsigned char>(0));
    Check(RestoreDebugSettings()&&f.Leaf()==0&&!entry.owned,"Nested restore overwrote external edit or retained ownership");
    Check(f.ApplyNested()&&f.Leaf()==1&&entry.original==0,"Nested edit did not adopt external baseline");
    Check(RestoreDebugSettings()&&f.Memory()==initial,"Nested external-baseline restore modified other bytes");
    fixtureContext=false;
}
void OriginalScalarPoliciesAndAliases() {
    Fixture f;
    f.Add("Short",DebugSettingType::Int16,5,0xfffe);
    f.specs[7].minimum=-32768; f.specs[7].maximum=32767;
    f.Put(f.specs[7].live+2,std::uint16_t(0xa55a));
    InitializeEntries(f.image,f.specs,{{"Test.Short","second.page","Original short label"},
        {"Test.Bool","second.page","Original Boolean label"},{"Unknown","second.page","Missing"}});
    const auto aliases=SnapshotDebugSettings();
    Check(aliases.size()==10&&settings.entries.size()==8,"Aliases created duplicate scalar owners or exposed an unknown ID");
    auto original=ReadDebugSetting("second.page","Original short label");
    Check(original.available&&original.id=="Test.Short"&&original.number==-2&&original.pageId=="second.page",
        "Signed short alias did not retain original row identity and shared action ID");
    Check(!ReadDebugSetting("second.page","Missing").available,"Unknown alias exposed a value");
    fixtureContext=false;Check(!ApplyDebugSetting(original.id,1)&&f.Read(7).number==-2,"Alias bypassed verified callback");
    fixtureContext=true;
    Check(ApplyDebugSetting(original.id,1)&&f.Read(7).number==-1,"Alias failed to update signed short");
    Check(f.Apply(7,1)&&ReadDebugSetting("second.page","Original short label").number==0,"Primary/alias edits diverged");
    Check(settings.entries[7].original==0xfffe&&settings.entries[7].owned,"Second alias edit replaced first ownership baseline");
    std::uint16_t neighbor=0;std::memcpy(&neighbor,f.bytes+f.specs[7].live+2,2);
    Check(neighbor==0xa55a,"Short atomic edit overwrote adjacent two bytes");
    Check(RestoreDebugSettings()&&f.Read(7).number==-2,"Shared alias restore did not restore first signed baseline");
    Check(f.Apply(7,1),"Cannot seed short external-edit case");
    f.Put(f.specs[7].live,std::int16_t(-9));
    Check(RestoreDebugSettings()&&f.Read(7).number==-9,"Short restore clobbered later external edit");
    Check(f.Apply(7,1)&&RestoreDebugSettings()&&f.Read(7).number==-9,"Short external baseline was not adopted");
    f.Put(f.specs[7].live,std::int16_t(32767));
    Check(!f.Apply(7,1)&&f.Read(7).number==32767,"Short increment overflow accepted");
    f.Put(f.specs[7].live,std::int16_t(-32768));
    Check(!f.Apply(7,-1)&&f.Read(7).number==-32768,"Short decrement overflow accepted");
    Check(f.Apply(7,1)&&f.Read(7).number==-32767&&RestoreDebugSettings(),"Short minimum boundary did not preserve signed value");
    auto& shortSpec=settings.entries[7].spec;
    shortSpec.minimum=0; f.Put(f.specs[7].live,std::int16_t(0));
    Check(!f.Apply(7,-1)&&f.Apply(7,1)&&f.Read(7).number==1&&RestoreDebugSettings(),"Original zero-based Int16 policy failed");
    shortSpec.maximum=32768;Check(!f.Apply(7,1),"Out-of-primitive short policy accepted");shortSpec.maximum=32767;
    shortSpec.step=0.5;Check(!f.Apply(7,1),"Fractional integer policy accepted");shortSpec.step=1;
    const auto oldLive=shortSpec.live;++shortSpec.live;
    f.Put(shortSpec.registration+8,f.image.base+shortSpec.live);
    Check(!f.Read(7).available&&!f.Apply(7,1),"Unaligned two-byte scalar accepted");
    shortSpec.live=oldLive;f.Put(shortSpec.registration+8,f.image.base+oldLive);
    std::uintptr_t address=0;Check(Validate(f.image,shortSpec,address),"Restored short identity rejected");
    Check(!CompareExchange(f.image,shortSpec,address,4,5)&&f.Read(7).number==0,"Short stale compare/exchange overwrote live value");
    auto& floatSpec=settings.entries[3].spec;
    floatSpec.minimum=-1;floatSpec.maximum=1;floatSpec.step=0.5;
    f.Put(f.specs[3].live,Fixture::FloatBits(0));
    Check(f.Apply(3,-1)&&f.Read(3).number==-0.5&&f.Apply(3,-1)&&f.Read(3).number==-1,
        "Verified negative float policy was silently clamped to nonnegative values");
    Check(!f.Apply(3,-1)&&RestoreDebugSettings()&&f.Read(3).number==0,"Negative float boundary or exact-zero restoration failed");
    floatSpec.minimum=std::numeric_limits<float>::min();floatSpec.maximum=std::numeric_limits<float>::max();floatSpec.step=0.1f;
    Check(!f.Apply(3,-1)&&f.Apply(3,1)&&f.Read(3).number>0&&RestoreDebugSettings()&&f.Read(3).number==0,
        "Original FLT_MIN policy incorrectly wrote below minimum or prevented restoring zero");
    for (double badStep : {0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        floatSpec.step=badStep;Check(!f.Apply(3,1),"Invalid scalar step accepted");
    }
    floatSpec.step=0.1;floatSpec.minimum=2;floatSpec.maximum=1;Check(!f.Apply(3,1),"Reversed scalar range accepted");
    floatSpec.minimum=-std::numeric_limits<double>::max();Check(!f.Apply(3,1),"Unrepresentable float policy accepted");
    auto& uintSpec=settings.entries[2].spec;uintSpec.minimum=-1;
    Check(!f.Apply(2,1),"Negative unsigned policy accepted");
    uintSpec.minimum=0;uintSpec.maximum=4294967296.0;
    Check(!f.Apply(2,1),"Oversized unsigned policy accepted");
    std::memcpy(&neighbor,f.bytes+f.specs[7].live+2,2);
    Check(neighbor==0xa55a,"Short boundary/restore matrix changed neighboring storage");
    std::uint32_t defaults=0;std::memcpy(&defaults,f.bytes+f.specs[7].defaults,4);
    Check(defaults==0x11111111,"Short operation mutated reflected default storage");
    fixtureContext=false;
}
void ProductionAllowlistPolicies() {
    Check(kBindings.size()==73&&kBindingAliases.size()==6,"Unexpected scalar or alias allowlist count");
    Check(kRawBooleans.size()==1&&kRawBooleans[0].id=="Reactions.DisableReactGraph"&&
        kRawBooleans[0].live==0xadf08c0&&kRawBooleans[0].consumer==0x69d220&&kRawBooleans[0].code.size()==0x84,
        "Unexpected raw Boolean allowlist or matched consumer range");
    for(std::size_t i=0;i<kBindings.size();++i) {
        const auto& a=kBindings[i];
        for(const auto& raw:kRawBooleans)Check(raw.id!=a.id&&(raw.live<a.live+a.offset||raw.live>=a.live+a.offset+Width(a)),
            "Raw Boolean overlaps a reflected owner");
        Check(a.id!="Engine.TriggerActivateThreshold","No-consumer trigger threshold was enabled");
        Check(a.id!="Health.HeroesDoNotTakeDamage"&&a.id!="Health.HeroesDoNotDie"&&
            a.id!="Health.EnemyBotsDoNotDie"&&a.id!="Health.EnemyBotsDontTakeDamage","Dedicated health byte has duplicate ownership");
        for(std::size_t j=0;j<i;++j) {
            const auto& b=kBindings[j];
            Check(a.id!=b.id,"Duplicate setting ownership ID");
            Check(a.live+a.offset+Width(a)<=b.live+b.offset||b.live+b.offset+Width(b)<=a.live+a.offset,
                "Scalar bindings overlap storage");
        }
    }
    for(const auto& alias:kBindingAliases)
        Check(std::count_if(kBindings.begin(),kBindings.end(),[&](const auto& b){return b.id==alias.id;})==1,
            "Alias does not resolve exactly one scalar owner");
}
void RawBooleanPolicy() {
    Fixture f;fixtureContext=false;
    auto spec=kRawBooleans[0];spec.live=0x8080;spec.consumer=0xb000;
    const auto displacement=static_cast<std::int32_t>(spec.live-spec.consumer-spec.readOffset-7);
    std::memcpy(spec.code.data()+spec.readOffset+2,&displacement,4);
    std::memcpy(f.bytes+spec.consumer,spec.code.data(),spec.code.size());
    f.Put(spec.live-1,static_cast<unsigned char>(0xa5));f.Put(spec.live,static_cast<unsigned char>(0));f.Put(spec.live+1,static_cast<unsigned char>(0x5a));
    f.image.executable.push_back({f.image.base+spec.consumer,0x1000});
    DWORD previous=0,ignored=0;
    Check(VirtualProtect(f.bytes+spec.consumer,0x1000,PAGE_EXECUTE_READ,&previous)!=FALSE,"Cannot protect synthetic consumer as execute/read");
    InitializeEntries(f.image,f.specs,{}, {spec});
    auto read=[&](){return ReadDebugSetting(spec.page,spec.label);};
    auto apply=[&](int direction){return ApplyDebugSetting(spec.id,direction);};
    const auto beforeRead=contextChecks;
    const auto snapshot=SnapshotDebugSettings();auto value=read();
    Check(snapshot.size()==8&&value.available&&value.id==spec.id&&value.type==DebugSettingType::Boolean&&value.number==0,
        "Raw Boolean snapshot did not expose exact original row metadata");
    Check(contextChecks==beforeRead,"Raw Boolean read entered callback context");
    Check(!apply(1)&&f.bytes[spec.live]==0&&!settings.rawBooleans[0].owned,"Raw Boolean wrote outside callback");
    fixtureContext=true;
    Check(apply(-1)&&!settings.rawBooleans[0].owned,"Unchanged raw value took ownership");
    Check(apply(1)&&f.bytes[spec.live]==1&&settings.rawBooleans[0].owned,"Raw Right did not enable");
    Check(apply(1)&&settings.rawBooleans[0].original==0,"Repeated raw Right lost baseline");
    Check(apply(0)&&f.bytes[spec.live]==0&&apply(0)&&f.bytes[spec.live]==1,"Raw activation did not toggle both directions");
    Check(f.bytes[spec.live-1]==0xa5&&f.bytes[spec.live+1]==0x5a,"Raw Boolean changed neighboring bytes");
    Check(!apply(2)&&!apply(-2)&&f.bytes[spec.live]==1,"Raw Boolean accepted an invalid direction");
    fixtureContext=false;Check(!RestoreDebugSettings()&&f.bytes[spec.live]==1,"Raw restore escaped callback gate");fixtureContext=true;
    Check(!ExchangeRawBoolean(settings.image,spec,0,0)&&f.bytes[spec.live]==1,"Raw stale compare/exchange clobbered an external value");
    Check(!ExchangeRawBoolean(settings.image,spec,1,2)&&f.bytes[spec.live]==1,"Raw compare/exchange accepted a non-Boolean");
    for(std::size_t index=0;index<spec.code.size();++index) {
        Check(VirtualProtect(f.bytes+spec.consumer,0x1000,PAGE_READWRITE,&ignored)!=FALSE,"Cannot alter synthetic consumer fixture");
        f.bytes[spec.consumer+index]^=1;
        Check(VirtualProtect(f.bytes+spec.consumer,0x1000,PAGE_EXECUTE_READ,&ignored)!=FALSE,"Cannot reseal synthetic consumer fixture");
        Check(!read().available&&!apply(-1)&&!RestoreDebugSettings()&&f.bytes[spec.live]==1&&settings.rawBooleans[0].owned,
            "Changed raw consumer code accepted a read/write or discarded pending restoration");
        Check(VirtualProtect(f.bytes+spec.consumer,0x1000,PAGE_READWRITE,&ignored)!=FALSE,"Cannot reset synthetic consumer fixture");
        f.bytes[spec.consumer+index]^=1;
        Check(VirtualProtect(f.bytes+spec.consumer,0x1000,PAGE_EXECUTE_READ,&ignored)!=FALSE,"Cannot reseal restored consumer fixture");
    }
    Check(RestoreDebugSettings()&&f.bytes[spec.live]==0&&!settings.rawBooleans[0].owned,"Raw deferred restoration failed after exact code recovery");
    Check(apply(1),"Cannot seed external raw edit");f.Put(spec.live,static_cast<unsigned char>(0));
    Check(RestoreDebugSettings()&&f.bytes[spec.live]==0&&!settings.rawBooleans[0].owned,"Raw restore overwrote later external edit");
    f.Put(spec.live,static_cast<unsigned char>(1));Check(apply(-1)&&f.bytes[spec.live]==0,"Raw Left did not disable");
    Check(RestoreDebugSettings()&&f.bytes[spec.live]==1,"Raw restore failed to preserve an initially enabled baseline");
    f.Put(spec.live,static_cast<unsigned char>(0));Check(apply(1),"Cannot seed invalid external byte case");f.Put(spec.live,static_cast<unsigned char>(2));
    Check(!read().available&&!apply(0)&&!RestoreDebugSettings()&&f.bytes[spec.live]==2,"Invalid raw live byte was normalized or overwritten");
    f.Put(spec.live,static_cast<unsigned char>(1));Check(RestoreDebugSettings()&&f.bytes[spec.live]==0,"Raw invalid-byte deferred restore could not recover");
    auto& liveSpec=settings.rawBooleans[0].spec;
    liveSpec.live++;Check(!read().available&&!apply(1),"Raw Boolean accepted a different target than its consumer RIP read");liveSpec=spec;
    liveSpec.readOffset++;Check(!read().available&&!apply(1),"Raw Boolean accepted an invalid instruction boundary");liveSpec=spec;
    liveSpec.code.clear();Check(!read().available&&!apply(1),"Raw Boolean accepted an empty code pin");liveSpec=spec;
    settings.image.executable.clear();Check(!read().available&&!apply(1),"Raw consumer outside executable image section accepted");settings.image=f.image;
    settings.image.writable.clear();Check(!read().available&&!apply(1),"Raw byte outside writable image section accepted");settings.image=f.image;
    Check(VirtualProtect(f.bytes+spec.consumer,0x1000,PAGE_READWRITE,&ignored)!=FALSE,"Cannot test non-executable consumer page");
    Check(!read().available&&!apply(1),"Raw consumer with changed executable protection accepted");
    Check(VirtualProtect(f.bytes+spec.consumer,0x1000,PAGE_EXECUTE_READ,&ignored)!=FALSE,"Cannot restore code page protection");
    Check(VirtualProtect(f.bytes+0x8000,0x1000,PAGE_READONLY,&ignored)!=FALSE,"Cannot test read-only raw value page");
    Check(!read().available&&!apply(1),"Raw Boolean read-only value page accepted");
    Check(VirtualProtect(f.bytes+0x8000,0x1000,PAGE_READWRITE,&ignored)!=FALSE,"Cannot restore raw data page protection");
    Check(read().available&&f.bytes[spec.live-1]==0xa5&&f.bytes[spec.live+1]==0x5a,"Raw rejection cases damaged adjacent bytes or final metadata");
}
void OriginalCustomAdjustmentPolicies() {
    Fixture f;fixtureContext=true;
    auto& boolean=settings.entries[0].spec;
    boolean.adjustment=AdjustmentPolicy::PositiveBoolean;
    Check(f.Apply(0,0)&&f.Bits(0)==0&&!settings.entries[0].owned,"Native positive-only setter toggled on a zero direction");
    Check(f.Apply(0,1)&&f.Bits(0)==1,"Native positive-only setter did not enable on right");
    Check(f.Apply(0,1)&&f.Bits(0)==1&&settings.entries[0].original==0,"Repeated native enable replaced ownership baseline");
    Check(f.Apply(0,0)&&f.Bits(0)==0,"Native positive-only setter did not disable on zero direction");
    Check(f.Apply(0,1)&&f.Apply(0,-1)&&f.Bits(0)==0,"Native positive-only setter did not disable on left");
    Check(RestoreDebugSettings()&&f.Bits(0)==0,"Native Boolean setter failed owned restoration");
    boolean.adjustment=AdjustmentPolicy::HalveDouble;
    Check(!f.Apply(0,1),"Numeric custom adjustment accepted a Boolean");
    boolean.adjustment=static_cast<AdjustmentPolicy>(99);
    Check(!f.Apply(0,1),"Unknown custom adjustment accepted");
    boolean.adjustment=AdjustmentPolicy::Default;
    auto& terrain=settings.entries[2].spec;
    terrain.minimum=8;terrain.maximum=256;terrain.step=1;terrain.adjustment=AdjustmentPolicy::HalveDouble;
    f.Put(f.specs[2].live,std::uint32_t(16));
    const std::uint32_t expected[]{32,64,128,256,256};
    for(auto value:expected)Check(f.Apply(2,1)&&f.Bits(2)==value,"Native terrain doubling/clamped maximum differs");
    for(auto value:{128u,64u,32u,16u,8u,8u})Check(f.Apply(2,-1)&&f.Bits(2)==value,"Native terrain halving/clamped minimum differs");
    Check(!f.Apply(2,0)&&f.Bits(2)==8,"Directionless terrain activation changed density");
    Check(RestoreDebugSettings()&&f.Bits(2)==16,"Terrain adjustment lost first original density");
    f.Put(f.specs[2].live,std::uint32_t(19));
    Check(f.Apply(2,-1)&&f.Bits(2)==9&&f.Apply(2,-1)&&f.Bits(2)==8,"Native unsigned halving did not truncate odd densities");
    Check(RestoreDebugSettings()&&f.Bits(2)==19,"Odd external density was not restored exactly");
    f.Put(f.specs[2].live,std::uint32_t(257));Check(!f.Apply(2,1)&&f.Bits(2)==257,"Invalid live terrain density was guessed or overflowed");
    f.Put(f.specs[2].live,std::uint32_t(16));Check(f.Apply(2,1),"Cannot seed terrain external-edit case");
    f.Put(f.specs[2].live,std::uint32_t(64));Check(RestoreDebugSettings()&&f.Bits(2)==64,"Terrain restore overwrote later external edit");
    terrain.adjustment=AdjustmentPolicy::PositiveBoolean;Check(!f.Apply(2,1),"Boolean custom adjustment accepted integer storage");
    terrain.adjustment=AdjustmentPolicy::HalveDouble;fixtureContext=false;
    Check(!f.Apply(2,1)&&f.Bits(2)==64,"Native custom adjustment bypassed dispatcher scope");
}
}
int main(){try{
    Exercise();RestrictedEnumPolicy();const auto legacyChecks=checks;
    NestedParentPolicy();const auto nestedChecks=checks-legacyChecks;
    OriginalScalarPoliciesAndAliases();const auto policyChecks=checks-legacyChecks-nestedChecks;
    OriginalCustomAdjustmentPolicies();const auto customChecks=checks-legacyChecks-nestedChecks-policyChecks;
    RawBooleanPolicy();const auto rawChecks=checks-legacyChecks-nestedChecks-policyChecks-customChecks;
    ProductionAllowlistPolicies();
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"legacy_checks\":"<<legacyChecks
        <<",\"nested_checks\":"<<nestedChecks<<",\"original_policy_checks\":"<<policyChecks
        <<",\"custom_adjustment_checks\":"<<customChecks
        <<",\"raw_boolean_checks\":"<<rawChecks<<",\"raw_boolean_bindings\":"<<sm2debug::kRawBooleans.size()
        <<",\"nested_rejection_cases\":"<<nestedRejectionCases<<",\"allowlisted_reference_bindings\":"<<sm2debug::kBindings.size()
        <<",\"shared_owner_aliases\":"<<sm2debug::kBindingAliases.size()
        <<",\"restricted_enum_policy_verified\":true,\"int16_atomic_and_neighbor_preservation_verified\":true,\"signed_and_original_scalar_limits_verified\":true,\"alias_shared_ownership_verified\":true,\"nested_parent_identity_verified\":true,\"nested_nonzero_leaf_offset_verified\":true,\"nested_neighbor_and_default_bytes_preserved\":true,\"nested_deferred_restore_verified\":true,\"nested_external_edit_preserved\":true,\"game_loaded\":false,\"game_started\":false}\n";return 0;}
catch(const std::exception& error){std::cout<<"{\"passed\":false,\"checks\":"<<checks<<",\"error\":\""<<error.what()<<"\"}\n";return 1;}}
