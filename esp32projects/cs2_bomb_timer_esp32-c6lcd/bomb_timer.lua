--[[
--  @title
--      bomb_timer.lua
--
--  @protocol (9-field CSV):
--      ticking,defused,time_left,timer_length,being_defused,has_kit,hp_after,defuse_time_left,planting
--      Example: "1,0,25.30,40.00,1,1,73,3.20,0"
--]]

local modules = require( "modules" )
local players = require( "players" )
local math_sqrt = math.sqrt
local math_floor = math.floor
local math_max = math.max
local math_exp = math.exp

local bomb_timer = {}

local HTTP_HOST = "0.0.0.0"
local HTTP_PORT = 8888

-- map bomb damage table
local bomb_params_by_map = {
    ["de_dust2"]   = { damage = 500, radius = 1750 },
    ["de_ancient"] = { damage = 650, radius = 2275 },
    ["de_anubis"]  = { damage = 450, radius = 1575 },
    ["de_inferno"] = { damage = 620, radius = 2170 },
    ["de_mirage"]  = { damage = 650, radius = 2275 },
    ["de_nuke"]    = { damage = 650, radius = 2275 },
    ["de_overpass"]= { damage = 650, radius = 2275 },
    ["de_vertigo"] = { damage = 500, radius = 1750 },
    ["de_train"]   = { damage = 650, radius = 2275 },
}

local function get_bomb_params( map )
    return bomb_params_by_map[ map ] or { damage = 650, radius = 2275 }
end

-- state
bomb_timer.state = {
    bomb_ticking     = false,
    bomb_defused     = false,
    time_left        = 0.0,
    timer_length     = 40.0,
    being_defused    = false,
    has_kit          = false,
    hp_after         = -1,
    defuse_time_left = -1.0,
    planting         = false,
}

bomb_timer.offsets = {}
bomb_timer.offsets_cached = false

function bomb_timer.on_solution_calibrated( data )
    if data.gameid ~= GAME_CS2 then
        fantasy.log( "[bomb_timer] CS2 only. Disabling." )
        return false
    end
    fantasy.log( "[bomb_timer] CS2 detected." )
end

function bomb_timer.cache_offsets()
    local src2 = modules.source2

    -- planted C4
    bomb_timer.offsets.bomb_ticking     = src2:get_schema( "C_PlantedC4", "m_bBombTicking" )
    bomb_timer.offsets.bomb_blow        = src2:get_schema( "C_PlantedC4", "m_flC4Blow" )
    bomb_timer.offsets.bomb_defused     = src2:get_schema( "C_PlantedC4", "m_bBombDefused" )
    bomb_timer.offsets.timer_length     = src2:get_schema( "C_PlantedC4", "m_flTimerLength" )
    bomb_timer.offsets.being_defused    = src2:get_schema( "C_PlantedC4", "m_bBeingDefused" )
    bomb_timer.offsets.defuse_countdown = src2:get_schema( "C_PlantedC4", "m_flDefuseCountDown" )
    bomb_timer.offsets.defuse_length    = src2:get_schema( "C_PlantedC4", "m_flDefuseLength" )

    -- carried C4 (for planting detection)
    bomb_timer.offsets.started_arming   = src2:get_schema( "C_C4", "m_bStartedArming" )

    -- positions
    bomb_timer.offsets.scene_node       = src2:get_schema( "C_BaseEntity", "m_pGameSceneNode" )
    bomb_timer.offsets.abs_origin       = src2:get_schema( "CGameSceneNode", "m_vecAbsOrigin" )

    -- armor
    bomb_timer.offsets.armor_value      = src2:get_schema( "C_CSPlayerPawn", "m_ArmorValue" )

    if bomb_timer.offsets.bomb_ticking then
        bomb_timer.offsets_cached = true
        fantasy.log( "[bomb_timer] Schema offsets cached OK" )
        if bomb_timer.offsets.started_arming then
            fantasy.log( "[bomb_timer] C_C4 planting detection available" )
        else
            fantasy.log( "[bomb_timer] C_C4 planting detection NOT available (will skip)" )
        end
        return true
    end

    return false
end

-- helpers
local function get_entity_origin( ent )
    if not bomb_timer.offsets.scene_node then return nil end
    local node = ent:read( MEM_ADDRESS, bomb_timer.offsets.scene_node )
    if not node or not node:is_valid() then return nil end
    return node:read( MEM_VECTOR, bomb_timer.offsets.abs_origin )
end

local function calc_distance( a, b )
    if not a or not b then return nil end
    local dx = b.x - a.x
    local dy = b.y - a.y
    local dz = b.z - a.z
    return math_sqrt( dx*dx + dy*dy + dz*dz )
end

local function calc_hp_after_bomb( bomb_ent, player )
    local bomb_pos = get_entity_origin( bomb_ent )
    local player_pos = player:get_origin()
    if not bomb_pos or not player_pos then return -1 end

    local globals = modules.source2:get_globals()
    if not globals or not globals.map then return -1 end
    local map_name = globals.map:gsub( "^.*/", "" ):gsub( "^.*\\", "" )
    local params = get_bomb_params( map_name )

    local health = player:get_health()
    local armor  = 0
    if bomb_timer.offsets.armor_value then
        armor = player:read( MEM_INT, bomb_timer.offsets.armor_value ) or 0
    end

    local dist = calc_distance( player_pos, bomb_pos )
    if not dist then return -1 end

    local sigma = params.radius / 3.0
    local damage = params.damage * math_exp( -(dist * dist) / (2.0 * sigma * sigma) )

    if armor > 0 then
        local reduced = damage / 2.0
        local armor_dmg = (damage - reduced) / 2.0
        if armor_dmg > armor then
            reduced = damage - (armor * 2.0)
        end
        damage = reduced
    end

    return math_max( 0, health - math_floor( damage ) )
end

function bomb_timer.on_loaded( data )
    fantasy.log( "[bomb_timer] Binding HTTP on port {}...", HTTP_PORT )
    local http = fantasy.http()
    http:new( HTTP_HOST, HTTP_PORT )
    fantasy.log( "[bomb_timer] HTTP bound. Poll /luar on port {}.", HTTP_PORT )
    bomb_timer.cache_offsets()
end

function bomb_timer.on_worker( is_calibrated, game_id )

    if not is_calibrated then
        bomb_timer.state.bomb_ticking = false
        bomb_timer.state.planting = false
        return
    end

    if not bomb_timer.offsets_cached then
        if not bomb_timer.cache_offsets() then return end
    end

    local globals = modules.source2:get_globals()
    if not globals then return end
    local cur_time = globals.tick_count * 0.015625

    local entities = modules.entity_list:get_entities()
    if not entities then
        bomb_timer.state.bomb_ticking = false
        bomb_timer.state.planting = false
        return
    end

    -- local player for damage calc
    local local_player = nil
    local lp_raw = modules.entity_list:get_localplayer()
    if lp_raw then
        local ok, p = pcall( players.to_player, lp_raw )
        if ok then local_player = p end
    end

    -- check for planting (carried C4)
    bomb_timer.state.planting = false
    if bomb_timer.offsets.started_arming then
        for _, ent in pairs( entities ) do
            if ent.class and ( ent.class == "C_C4" or ent.class == "CC4" ) then
                local arming = ent:read( MEM_BOOL, bomb_timer.offsets.started_arming )
                if arming then
                    bomb_timer.state.planting = true
                end
                break
            end
        end
    end

    -- check for planted bomb
    local found = false

    for _, ent in pairs( entities ) do
        if ent.class and ( ent.class == "C_PlantedC4" or ent.class == "CPlantedC4" ) then

            local ticking = ent:read( MEM_BOOL, bomb_timer.offsets.bomb_ticking )
            local defused = ent:read( MEM_BOOL, bomb_timer.offsets.bomb_defused )

            if ticking and not defused then
                local blow_time    = ent:read( MEM_FLOAT, bomb_timer.offsets.bomb_blow )
                local timer_len    = ent:read( MEM_FLOAT, bomb_timer.offsets.timer_length )
                local is_defusing  = ent:read( MEM_BOOL,  bomb_timer.offsets.being_defused )
                local defuse_len   = ent:read( MEM_FLOAT, bomb_timer.offsets.defuse_length )
                local defuse_cd    = ent:read( MEM_FLOAT, bomb_timer.offsets.defuse_countdown )

                local remaining = blow_time - cur_time
                if remaining < 0 then remaining = 0 end

                local kit = false
                if is_defusing and defuse_len and defuse_len < 7.0 then
                    kit = true
                end

                local defuse_tleft = -1.0
                if is_defusing and defuse_cd then
                    defuse_tleft = defuse_cd - cur_time
                    if defuse_tleft < 0 then defuse_tleft = 0 end
                end

                local hp = -1
                if local_player then
                    local ok, result = pcall( calc_hp_after_bomb, ent, local_player )
                    if ok then hp = result end
                end

                bomb_timer.state.bomb_ticking     = true
                bomb_timer.state.bomb_defused     = false
                bomb_timer.state.time_left        = remaining
                bomb_timer.state.timer_length     = timer_len or 40.0
                bomb_timer.state.being_defused    = is_defusing or false
                bomb_timer.state.has_kit          = kit
                bomb_timer.state.hp_after         = hp
                bomb_timer.state.defuse_time_left = defuse_tleft
                bomb_timer.state.planting         = false  -- can't be planting if already planted

            elseif defused then
                bomb_timer.state.bomb_ticking     = false
                bomb_timer.state.bomb_defused     = true
                bomb_timer.state.time_left        = 0
                bomb_timer.state.being_defused    = false
                bomb_timer.state.has_kit          = false
                bomb_timer.state.hp_after         = -1
                bomb_timer.state.defuse_time_left = -1.0
            else
                bomb_timer.state.bomb_ticking     = false
                bomb_timer.state.bomb_defused     = false
                bomb_timer.state.time_left        = 0
                bomb_timer.state.being_defused    = false
                bomb_timer.state.has_kit          = false
                bomb_timer.state.hp_after         = -1
                bomb_timer.state.defuse_time_left = -1.0
            end

            found = true
            break
        end
    end

    if not found then
        bomb_timer.state.bomb_ticking     = false
        bomb_timer.state.bomb_defused     = false
        bomb_timer.state.time_left        = 0
        bomb_timer.state.being_defused    = false
        bomb_timer.state.has_kit          = false
        bomb_timer.state.hp_after         = -1
        bomb_timer.state.defuse_time_left = -1.0
    end
end

function bomb_timer.on_http_request( data )
    if data["path"] ~= "/luar" then return end

    return fantasy.fmt(
        "{},{},{:.2f},{:.2f},{},{},{},{:.2f},{}",
        bomb_timer.state.bomb_ticking     and 1 or 0,
        bomb_timer.state.bomb_defused     and 1 or 0,
        bomb_timer.state.time_left,
        bomb_timer.state.timer_length,
        bomb_timer.state.being_defused    and 1 or 0,
        bomb_timer.state.has_kit          and 1 or 0,
        bomb_timer.state.hp_after,
        bomb_timer.state.defuse_time_left,
        bomb_timer.state.planting         and 1 or 0
    )
end

return bomb_timer
