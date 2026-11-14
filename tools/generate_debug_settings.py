"""Generate the explicit reference-to-retail scalar binding allowlist.

This does not expose all reflected fields. Entries name reference controls or
reviewed retail additions and a recovered live global registration/DDL field.
Separately proven raw Booleans use their own consumer-code pins, never DDL.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from supported_builds import CANONICAL_SHA256, is_supported_hash

PROJECT = Path(__file__).resolve().parent.parent
ROOT = PROJECT.parent
EXPECTED = CANONICAL_SHA256  # Original descriptor/proof provenance is unchanged.
RAW_BOOLEANS = [{
    'id':'Reactions.DisableReactGraph','page':'debug.reactions','label':'Disable React Graph',
    'type':'Boolean','live_rva':0xadf08c0,'consumer_rva':0x69d220,'read_offset':10,
    'consumer_code':'48895c2408574883ec20803d8f36750a00488bda488bf97560488bc248c1e82085c0780f488b5c24304883c4205fe98d050000488b01ff9098000000488d8f30030000e848053202488bf84885c0741c488b00488bcfff5048488bc8488d152d31750ae8a8d2310284c0750233ff488bd3488bcfe8d791ffff488b5c24304883c4205fc3',
    'source_global':'i30.exe+0x4235891','source_consumer':'i30.exe+0x5635b8..0x56366f',
    'semantics':'Left sets false, Right true, activation toggles; true prevents reaction graph assignment in the matched retail consumer.',
    'evidence':'verification/full-reimplementation-progress/gameplay-reactions-contract.json'
}]
SPEC = importlib.util.spec_from_file_location('pe_reader', ROOT / 'LTSC2019Compatibility/verification/read_minidump.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

# Page, exact reference row, registered group, exact reflected field.
MATCHES = [
 ('debug.prefab_actor_systems','All systems enabled','ManagedPrefabActor','AllSystemsEnabled'),
 ('debug.weapons','Infinite ammo','Weapon','InfiniteAmmo'),
 ('debug.debug_draw','Show tile boundaries','EngineDebug','TileBoundaryEnable'),
 ('debug.script_messages','Prototype messages','EngineDebug','PrototypeMessageEnable'),
 ('debug.script_messages','Debug messages','EngineDebug','DebugMessageEnable'),
 ('debug.rendering','Motion blur','Render','MotionBlurEnable'),
 ('debug.rendering','Visual effects processing','Engine','VFXEnable'),
 ('local.hero','Respawn Clear','Hero','RespawnClear'),
 ('debug.health','Draw health values','Health','DisplayHealthValues'),
 ('debug.health','Draw Hero Health Bar','Health','DisplayHeroHealthBar'),
 ('debug.health','Draw Enemy Health Bars','Health','DisplayEnemyHealthBars'),
 ('debug.health','Debug Health Drops','Health','DebugHealthDrops'),
 ('debug.health','Draw hazard areas','Health','DrawHazardAreas'),
 ('debug.health','Disable Hit Pause','Health','DisableHitPause'),
 ('debug.loading','Display System Info','LoadSystem','DisplaySystemInfo'),
 ('debug.loading','Display Streaming Unit Map','LoadSystem','DisplayOpenUnitMap'),
 ('debug.loading','Display Streaming Unit Distance','LoadSystem','DisplayOpenUnitDist'),
 ('debug.loading','Load Time Display Mode','LoadSystem','LoadTimeDisplayMode'),
 ('debug.loading','Lighting Zones Enabled','LoadSystem','LightingZonesEnabled'),
 ('debug.loading','Open World Streaming Enabled','LoadSystem','OpenUnitStreamingEnabled'),
 ('debug.loading','Overlay Loading Enabled','LoadSystem','OverlayLoadingEnabled'),
 ('debug.loading','Enable Delayed Streaming','LoadSystem','DelayStreamingEnabled'),
 ('debug.loading','Pause Delayed Streaming','LoadSystem','DelayStreamingPaused'),
 ('debug.loading','Force Longplay Reloads','LoadSystem','ForceLongplayReload'),
 ('debug.loading','DisableAllPrefabSpawning','Debug','DisableAllPrefabSpawning'),
 ('debug.loading','Texture Streaming','Render','TextureStreamingEnabled'),
 ('debug.loading','EnableSoundStreaming','Sound','EnableSoundStreaming'),
 ('debug.loading','LoadHistoryDetailLevel','AssetManager','LoadHistoryDetailLevel'),
 ('debug.loading','DisplayLooseLoadDetail','AssetManager','DisplayLooseLoadDetail'),
 ('debug.loading','ZoneInstantiationBudgetMin','EngineAmortization','ZoneInstantiationBudgetMin'),
 ('debug.loading','ZoneInstantiationBudgetMax','EngineAmortization','ZoneInstantiationBudgetMax'),
 ('debug.loading','AssetInstantiationBudgetMin','EngineAmortization','AssetInstantiationBudgetMin'),
 ('debug.loading','AssetInstantiationBudgetMax','EngineAmortization','AssetInstantiationBudgetMax'),
 ('debug.loading','AssetDeletionBudgetMin','EngineAmortization','AssetDeletionBudgetMin'),
 ('debug.loading','AssetDeletionBudgetMax','EngineAmortization','AssetDeletionBudgetMax'),
 ('debug.loading','ZoneInstantiationUseLeftoverFrameTime','EngineAmortization','ZoneInstantiationUseLeftoverFrameTime'),
 ('debug.loading','Instantiation Urgency Start Dist','LoadSystem','InstantiationUrgencyStartDist'),
 ('debug.loading','Instantiation Urgency End Dist','LoadSystem','InstantiationUrgencyEndDist'),
 ('debug.loading','Large Load Threshold (bytes)','LoadSystem','LargeLoadThresholdBytes'),
 ('debug.loading','ExpediteLoadStateMachineMode','SharedLoadSettings','ExpediteLoadStateMachineMode'),
 ('debug.loading','Loading TTY Verbosity','LoadSystem','LogVerbosity'),
 ('debug.gamecontrols','BubbleSkipOption','SpeedRun','BubbleSkipOption'),
 ('debug.gamecontrols','AllowFastTravelDuringDialog','SpeedRun','AllowFastTravelDuringDialog'),
 ('debug.gamecontrols','AllowCharacterSwitchDuringDialog','SpeedRun','AllowCharacterSwitchDuringDialog'),
]
# Original constructor arguments, source field identity, and retail readers are
# independently recorded in verification/retail-reimplementation/gameplay-binding-contract.json.
REIMPLEMENTED_MATCHES = [
 ('debug.rendering.lighting', 'Enable Subsurface Brdf', 'Render', 'EnableSubsurfaceBrdf'),
 ('debug.rendering.lighting.shadows.csmshadows', 'SDSM Depth Bias', 'Render', 'CsmSdsmDepthBias'),
 ('debug.rendering.models', 'LoD Transition Time', 'Render', 'ModelLoDTransitionTime'),
 ('debug.rendering.models', 'Adjust Depth Pre-Pass', 'Render', 'DepthPrePassBias'),
 ('debug.rendering.terrain', 'Render Mesh Start Level', 'Render', 'TerrainMeshRenderStartLevel'),
 ('debug.rendering.terrain', 'Shadow Mesh Start Level', 'Render', 'TerrainMeshShadowsStartLevel'),
 ('local.engine.assetmanager', 'Fail All Loads', 'EngineDebug', 'FailAllAssetLoads'),
 ('local.engine.assetmanager', 'Fail As Missing', 'EngineDebug', 'FailAssetsAsMissing'),
 ('local.engine.physics', 'Skip keyframing nonmoving objects', 'Physics', 'RagdollUpdateSkipUnmovedObjects'),
 ('debug.rendering.lighting', 'Enable Ambient Shadows', 'Render', 'EnableAmbientShadows'),
 ('debug.rendering.lighting.envprobe', 'RTEP Far Clip', 'RtEnvProbeSettings', 'FarClip'),
 ('debug.rendering.lighting.lightgrid', 'LightGridFadeTime', 'Render', 'LightGridFadeTime'),
 ('local.engine.physics', 'WarnOnThisManyManifoldsGloballyInOneFrame', 'PhysicsDiagnostics', 'WarnOnThisManyManifoldsGloballyInOneFrame'),
 ('local.engine.physics', 'WarnOnThisManyBodyActivationsGloballyInOneFrame', 'PhysicsDiagnostics', 'WarnOnThisManyBodyActivationsGloballyInOneFrame'),
 ('local.engine.physics', 'MinimumSecondsBetweenExcessiveManifoldReports', 'PhysicsDiagnostics', 'MinimumSecondsBetweenExcessiveManifoldReports'),
 ('local.engine.saveload', 'TTY Verbosity', 'Save', 'DebugVerbosity'),
 ('debug.profiling.gpuprofiling', 'Extra Flushes Enabled', 'Render', 'ExtraFlushesEnabled'),
 ('debug.game_systems.difficulty', 'DynamicDifficulty', 'Difficulty', 'DynamicDifficulty'),
 ('debug.game_systems.gameui', 'MapARBeacons', 'GameUI', 'MapARBeacons'),
 ('debug.game_systems.openworldactivities', 'ARBeacons', 'GameUI', 'ARBeacons'),
 ('debug.game_systems.partialragdoll', 'ManagerMode', 'PartialRagdoll', 'ManagerMode'),
 ('debug.game_systems.progression', 'TechboxOutline', 'ProgressionSystem', 'TechboxOutline'),
 ('debug.enemies.kraven', 'Swap crossbow net actor', 'Bot', 'EnableCrossbowNetSwap'),
 ('debug.sonytest', 'Print error markers', 'GUI', 'PrintErrorMarkers'),
 ('debug.traffic.vehicles', 'VerboseRidableDebug', 'TrafficShared', 'VerboseRidableDebug'),
 ('debug.weapons', 'Tap Max Hold Time', 'Weapon', 'TapMaxHoldTime'),
 ('local.engine.physics', 'Enable Undersampling on Faraway Ragdolls', 'Physics', 'EnableUnderSampleUpdatingRagdolls'),
 ('debug.rendering.terrain', 'Quads Per Meter', 'Render', 'TerrainQuadsPerMeter'),
]
# (minimum, maximum, small increment), recovered from original AddFloat/
# AddRangedSetting callers or the Engine.dll AddSetting primitive overload.
# FLT_MIN in the last two policies is the original positive-normal lower bound.
ORIGINAL_POLICIES = {
 'Render.CsmSdsmDepthBias': (0, 64.0, 0.10000000149011612),
 'Render.ModelLoDTransitionTime': (0, 5.0, 0.0010000000474974513),
 'Render.DepthPrePassBias': (0, 0.05000000074505806, 9.999999747378752e-05),
 'Render.TerrainMeshRenderStartLevel': (0, 16, 1),
 'Render.TerrainMeshShadowsStartLevel': (0, 16, 1),
 'RtEnvProbeSettings.FarClip': (1.0, 10000.0, 1.0),
 'Render.LightGridFadeTime': (0, 5.0, 0.10000000149011612),
 'PhysicsDiagnostics.WarnOnThisManyManifoldsGloballyInOneFrame': (0, 32767, 1),
 'PhysicsDiagnostics.WarnOnThisManyBodyActivationsGloballyInOneFrame': (0, 32767, 1),
 'PhysicsDiagnostics.MinimumSecondsBetweenExcessiveManifoldReports': (1.1754943508222875e-38, 3.4028234663852886e+38, 0.10000000149011612),
 'Weapon.TapMaxHoldTime': (1.1754943508222875e-38, 3.4028234663852886e+38, 0.10000000149011612),
 'Render.TerrainQuadsPerMeter': (8, 256, 1),
}
ADJUSTMENTS = {
 'Physics.EnableUnderSampleUpdatingRagdolls': 'PositiveBoolean',
 'Render.TerrainQuadsPerMeter': 'HalveDouble',
}
# Multiple original rows share one atomic ownership/restoration entry.
ALIASES = [
 ('EngineDebug.TileBoundaryEnable', 'debug.rendering.global', 'Show tile boundaries'),
 ('Render.MotionBlurEnable', 'debug.rendering.post', 'Enable Motion Blur'),
 ('Health.DisplayHealthValues', 'debug.health', 'Draw Health Values'),
 ('Weapon.InfiniteAmmo', 'debug.weapons', 'Infinite Ammo'),
 ('GUI.PrintErrorMarkers', 'debug.debug_draw', 'Print error markers'),
 ('EngineDebug.PrototypeMessageEnable', 'debug.debug_draw', 'Prototype Messages'),
]
TYPES = {15: ('Boolean',1), 5: ('Int16',2), 6: ('Int32',4), 2: ('UInt32',4), 8: ('Float32',4), 11: ('Enum32',4)}
# Explicitly reviewed inline child descriptors. This is not recursive exposure
# of arbitrary reflected fields; both parent and child are validated at runtime.
NESTED_MATCHES = [
 ('debug.traffic.pedestrians', 'Enable Pedestrians', 'Traffic', 'PedestrianLoader',
  'Enabled', 0x9eac490, 0x0e1e6595),
]


def cpp(text):
    result='"'
    for value in text.encode('utf-8'):
        result += ('\\'+chr(value)) if value in (34,92) else chr(value) if 32<=value<127 else '\\%03o'%value
    return result+'"'


def main():
    image=MODULE.PE(ROOT/'Spider-Man2.exe')
    actual_executable_hash=hashlib.sha256(image.data).hexdigest()
    if not is_supported_hash(actual_executable_hash):
        raise ValueError('Unsupported executable; do not regenerate guessed bindings')
    inventory=json.loads((PROJECT/'verification/retained-debug-settings.json').read_text())
    records={r['menu']:r for r in inventory['records']}
    def qword(rva): return struct.unpack('<Q',image.at(rva,8))[0]
    def string(pointer): return image.at(pointer-image.preferred_base,180).split(b'\0')[0].decode('ascii')
    def file_rva(offset):
        for vs,va,rs,ra in image.sections:
            if ra<=offset<ra+rs:return va+offset-ra
        raise ValueError('File offset outside image')
    def enum_info(type_id):
        candidates=[];position=0
        while True:
            position=image.data.find(struct.pack('<I',type_id),position)
            if position<0:break
            try:
                start=file_rva(position)-8
                count=struct.unpack('<I',image.at(start+12,4))[0]
                if not 1<=count<=64:raise ValueError('not an enum descriptor')
                names=qword(start+0x28);displays=qword(start+0x38)
                choices=[{'value':i,'name':string(qword(names-image.preferred_base+i*8)),
                          'label':string(qword(displays-image.preferred_base+i*8))} for i in range(count)]
                if type_id == 0xb5e7b894:
                    if [c['name'] for c in choices] != ['Off','On','NoReload']:
                        raise ValueError('Unexpected Weapon.InfiniteAmmo enum members')
                elif any(not c['name'].startswith('k') for c in choices):raise ValueError('not named enum members')
                candidates.append({'rva':start,'type_id':type_id,'choices':choices})
            except (ValueError,struct.error,UnicodeDecodeError):pass
            position+=1
        if len(candidates)!=1:raise ValueError('Enum descriptor not unique: '+hex(type_id))
        return candidates[0]
    def child_descriptor(rva):
        size=struct.unpack('<I',image.at(rva+0x14,4))[0]
        count=struct.unpack('<H',image.at(rva+0x18,2))[0]
        if not 0<size<=1024*1024 or not 0<count<=4096:raise ValueError('Invalid nested descriptor shape')
        names,codes,counts,offsets=[qword(rva+x)-image.preferred_base for x in (0x60,0x68,0x80,0x88)]
        return {'name':string(qword(rva)),'rva':hex(rva),'size':size,'field_count':count,
            'default_instance_rva':hex(qword(rva+0x50)-image.preferred_base),
            'fields':[{'name':string(qword(names+i*8)),'type_code':image.at(codes+i,1)[0],
                       'count':struct.unpack('<I',image.at(counts+i*4,4))[0],
                       'offset':struct.unpack('<I',image.at(offsets+i*4,4))[0]} for i in range(count)]}
    matches=[(page,label,group,field,None) for page,label,group,field in MATCHES+REIMPLEMENTED_MATCHES]
    matches.extend((page,label,group,field,(parent,rva,type_id))
        for page,label,group,parent,field,rva,type_id in NESTED_MATCHES)
    bindings=[]
    for page,label,group,field_name,nested in matches:
        record=records[group];descriptor=record['descriptor'];parent=None
        live_rva=int(record['live_rva'],16)
        if nested:
            parent_field,child_rva,type_id=nested
            parent_index=next(i for i,f in enumerate(descriptor['fields']) if f['name']==parent_field)
            field=descriptor['fields'][parent_index]
            desc_rva=int(descriptor['rva'],16)
            type_ids=qword(desc_rva+0x90)-image.preferred_base
            if field['type_code']!=13 or field['count']!=1 or \
                struct.unpack('<I',image.at(type_ids+parent_index*4,4))[0]!=type_id or \
                struct.unpack('<I',image.at(child_rva+8,4))[0]!=type_id:
                raise ValueError('Nested descriptor type identity mismatch')
            parent={'field':parent_field,'descriptor_name':descriptor['name'],'live_rva':live_rva,
                'descriptor_rva':desc_rva,'default_rva':int(descriptor['default_instance_rva'],16),
                'object_size':descriptor['size'],'field_count':descriptor['field_count'],
                'index':parent_index,'offset':field['offset'],'type_id':type_id}
            descriptor=child_descriptor(child_rva)
            if not 0<=parent['offset']<parent['object_size'] or descriptor['size']>parent['object_size']-parent['offset']:
                raise ValueError('Nested descriptor exceeds parent storage')
            live_rva+=parent['offset']
        fields=descriptor['fields']
        index=next(i for i,f in enumerate(fields) if f['name']==field_name);field=fields[index]
        kind,width=TYPES[field['type_code']]
        if field['count']!=1:raise ValueError('Complex/array binding refused')
        desc_rva=int(descriptor['rva'],16)
        item={'id':group+'.'+(parent['field']+'.' if parent else '')+field_name,'page':page,'label':label,'group':group,'field':field_name,'descriptor_name':descriptor['name'],
            'registration_rva':int(record['registration_name_pointer_rva'],16),'live_rva':live_rva,
            'descriptor_rva':desc_rva,'default_rva':int(descriptor['default_instance_rva'],16),'object_size':descriptor['size'],
            'field_count':descriptor['field_count'],'index':index,'offset':field['offset'],'type_code':field['type_code'],'type':kind,'width':width,
            'step':0.0001 if kind=='Float32' else 1,'minimum':0,
            'maximum':3.4028234663852886e38 if kind=='Float32' else 4294967295 if kind=='UInt32' else 2147483647,
            'range_source':'nonnegative scalar UI policy within native primitive range; not claimed engine limits',
            'peer_id':'','peer_relation':0,'enum':None,'parent':parent}
        if field_name.endswith('BudgetMin') or field_name.endswith('BudgetMax'):
            suffix='Max' if field_name.endswith('Min') else 'Min'
            item['peer_id']=group+'.'+field_name[:-3]+suffix
            item['peer_relation']=-1 if suffix=='Max' else 1
        if kind=='Enum32':
            type_ids=qword(desc_rva+0x90)-image.preferred_base
            type_id=struct.unpack('<I',image.at(type_ids+index*4,4))[0]
            item['enum']=enum_info(type_id)
            item['maximum']=len(item['enum']['choices'])-1
            item['range_source']='verified native enum ordinal range'
            if group == 'Weapon' and field_name == 'InfiniteAmmo':
                item['maximum']=1
                item['range_source']='Off/On runtime consumers verified; NoReload(2) remains readable/restorable but is not selected by this menu'
        if item['id'] in ORIGINAL_POLICIES:
            item['minimum'],item['maximum'],item['step']=ORIGINAL_POLICIES[item['id']]
            item['range_source']='verified original native UI arguments; verification/retail-reimplementation/gameplay-binding-contract.json'
        elif (page,label,group,field_name) in REIMPLEMENTED_MATCHES and kind not in ('Boolean','Enum32'):
            raise ValueError('Reimplemented scalar lacks original range policy: '+item['id'])
        item['adjustment']=ADJUSTMENTS.get(item['id'],'Default')
        if item['adjustment']!='Default':
            item['range_source']='exact original custom setter; verification/full-reimplementation-progress/gameplay-wave2-contract.json'
        bindings.append(item)
    by_id={b['id']:b for b in bindings}
    if len(by_id)!=len(bindings):raise ValueError('Duplicate scalar ownership ID')
    storage={(b['live_rva']+b['offset'],b['width']) for b in bindings}
    if len(storage)!=len(bindings):raise ValueError('Duplicate scalar storage ownership')
    row_keys={(b['page'],b['label']) for b in bindings}
    for ident,page,label in ALIASES:
        if ident not in by_id or (page,label) in row_keys:raise ValueError('Unknown or ambiguous row alias')
        row_keys.add((page,label))
    for raw in RAW_BOOLEANS:
        code=bytes.fromhex(raw['consumer_code']);start=raw['consumer_rva'];offset=raw['read_offset']
        if image.at(start,len(code))!=code or code[offset:offset+2]!=b'\x80\x3d' or code[offset+6]!=0 or \
                start+offset+7+struct.unpack('<i',code[offset+2:offset+6])[0]!=raw['live_rva']:
            raise ValueError('Raw Boolean consumer code/target mismatch: '+raw['id'])
        if raw['id'] in by_id or (raw['page'],raw['label']) in row_keys or any(
                b['live_rva']+b['offset']<=raw['live_rva']<b['live_rva']+b['offset']+b['width'] for b in bindings):
            raise ValueError('Raw Boolean overlaps another owner: '+raw['id'])
    manifest={'schema':3,'executable_sha256':EXPECTED,'actual_input_executable_sha256':actual_executable_hash,'binding_count':len(bindings),'bindings':bindings,
        'raw_boolean_count':len(RAW_BOOLEANS),'raw_booleans':RAW_BOOLEANS,
        'aliases':[{'id':ident,'page':page,'label':label} for ident,page,label in ALIASES],
        'reimplementation_evidence':'verification/retail-reimplementation/gameplay-binding-contract.json',
        'retail_addition_evidence':'verification/debug-runtime-expansion-bindings.json',
        'nested_field_evidence':'verification/traffic-pedestrians-binding-evidence.json',
        'prefab_control_evidence':'verification/prefab-system-binding-evidence.json',
        'unsupported':'Other reference rows remain unbound. Six Materials labels have no confirmed retained backend.',
        'enum_semantics':'DDL enum tables expose ordered, zero-based ordinal choices; current values must be valid ordinals before cycling.',
        'writes':'Only within IsWithinVerifiedEngineCallback; scalar atomic compare/exchange, restore only if current bytes still match this module last applied bytes.'}
    (PROJECT/'verification/debug-settings-bindings.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    lines=['// Generated by tools/generate_debug_settings.py; explicit observed-row allowlist only.',
           'const std::vector<BindingSpec> kBindings{']
    for b in bindings:
        enum=b['enum'];choices='{}' if not enum else '{'+', '.join('{'+str(c['value'])+', '+cpp(c['name'])+', '+cpp(c['label'])+'}' for c in enum['choices'])+'}'
        values=[cpp(b[k]) for k in ('id','page','label','group','field','descriptor_name')]
        values += [hex(b[k]) for k in ('registration_rva','live_rva','descriptor_rva','default_rva')]
        values += [str(b[k]) for k in ('object_size','field_count','index','offset','type_code')]
        values += ['DebugSettingType::'+b['type'],repr(b['step']),repr(float(b['minimum'])),repr(float(b['maximum'])),cpp(b['peer_id']),str(b['peer_relation']),hex(enum['rva']) if enum else '0',hex(enum['type_id']) if enum else '0',choices]
        if b['parent']:
            p=b['parent']
            values.append('{'+', '.join([cpp(p['field']),cpp(p['descriptor_name'])]+
                [hex(p[k]) for k in ('live_rva','descriptor_rva','default_rva')]+
                [str(p[k]) for k in ('object_size','field_count','index','offset','type_id')])+'}')
        if b['adjustment']!='Default':
            if not b['parent']:values.append('{}')
            values.append('AdjustmentPolicy::'+b['adjustment'])
        lines.append('    {'+', '.join(values)+'},')
    lines+=['};','const std::vector<BindingAlias> kBindingAliases{']
    lines += ['    {'+', '.join(cpp(value) for value in alias)+'},' for alias in ALIASES]
    lines+=['};','const std::vector<RawBooleanSpec> kRawBooleans{']
    for raw in RAW_BOOLEANS:
        values=[cpp(raw[k]) for k in ('id','page','label')]+[hex(raw[k]) for k in ('live_rva','consumer_rva')]+[str(raw['read_offset'])]
        values.append('{'+', '.join(hex(v) for v in bytes.fromhex(raw['consumer_code']))+'}')
        lines.append('    {'+', '.join(values)+'},')
    lines+=['};','']
    (PROJECT/'src/debug_settings_bindings.inc').write_text('\n'.join(lines),encoding='utf-8')
    print(json.dumps({'bindings':len(bindings),'raw_booleans':len(RAW_BOOLEANS),'enum_bindings':sum(b['enum'] is not None for b in bindings)}))


if __name__=='__main__':main()
