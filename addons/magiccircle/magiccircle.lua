addon.name    = 'magiccircle';
addon.author  = 'SQLCommit';
addon.version = '1.0.0';
addon.desc    = 'occlude3d example addon.';

require('common');
local struct   = require('struct');
local ffi      = require('ffi');
local d3d      = require('d3d8');
local imgui    = require('imgui');
local settings = require('settings');
local chat     = require('chat');

local C        = ffi.C;

local occ3d_ok, occ3d_warned = false, false;
local font_registered = false;
local function occ3d_present()
    if (AshitaCore:GetPluginManager():IsLoaded('occlude3d')) then
        if (not occ3d_ok) then occ3d_ok, occ3d_warned, font_registered = true, false, false; end
        return true;
    end
    occ3d_ok = false;
    if (not occ3d_warned) then
        occ3d_warned = true;
        print(chat.header(addon.name) .. chat.error('occlude3d plugin not loaded - this addon does nothing without it. Run: /load occlude3d'));
    end
    return false;
end

-- Constants + owner ids
local MAGIC      = 0x4F334432;
local O3DF_MAGIC = 0x4F334446;
local FONT_ID    = 0x4D434631;
local FONT_COLS, FONT_ROWS, FONT_FIRST, FONT_ADV = 16, 8, 32, 0.72;
local TEX_TRIS     = 5;
local TEX_TRIS_ADD = 5 + 0x100;
local TwoPi        = 2 * math.pi;

local OWNER_RUNE = 0x4D52554E;
local OWNER_RING = 0x4D52494E; 
local OWNER_BMA  = 0x4D424D41;
local OWNER_BMB  = 0x4D424D42;
local OWNER_BMC  = 0x4D424D43;
local OWNER_BMF  = 0x4D424D46;
local ALL_OWNERS = T{ OWNER_RUNE, OWNER_RING, OWNER_BMA, OWNER_BMB, OWNER_BMC, OWNER_BMF };

-- Settings
local default_settings = T{
    enabled = true,
    rune_on = true, rune_radius = 3.0, rune_spin = 0.30, rune_dirs = true, rune_col = T{ 0.45, 0.55, 1.00 },
    boom_on = true, boom_interval = 3.0, boom_count = 160, boom_size = 1.0, boom_power = 1.0,
    boom_height = 1.0, boom_smoke_h = 0.2, boom_life = 1.0, boom_smoke = true, boom_col = T{ 1.0, 0.65, 0.25 },
};
local s = settings.load(default_settings);
local SCALARS = T{ 'enabled',
    'rune_on','rune_radius','rune_spin','rune_dirs',
    'boom_on','boom_interval','boom_count','boom_size','boom_power','boom_height','boom_smoke_h','boom_life','boom_smoke' };
local COLORS  = T{ 'rune_col', 'boom_col' };
local function finite_num(v, dflt)
    v = tonumber(v);
    if (v == nil or v ~= v or v == math.huge or v == -math.huge) then return dflt; end
    return v;
end
local W = {};
for _, k in ipairs(SCALARS) do
    local d = default_settings[k];
    if (type(d) == 'boolean') then W[k] = { s[k] == true };
    else                          W[k] = { finite_num(s[k], d) }; end
end
for _, k in ipairs(COLORS) do
    local d, c = default_settings[k], s[k];
    W[k] = { finite_num(c and c[1], d[1]), finite_num(c and c[2], d[2]), finite_num(c and c[3], d[3]) };
end
local show, dirty = { false }, false;
local function sync_save()
    for _, k in ipairs(SCALARS) do s[k] = W[k][1]; end
    for _, k in ipairs(COLORS)  do s[k] = T{ W[k][1], W[k][2], W[k][3] }; end
    pcall(settings.save);
end

-- Textures
local circle_ptr, spark_ptr, smoke_ptr, font_ptr = 0, 0, 0, 0;
local refs, tex_attempts = {}, 0;
local function load_tex(name)
    local device = d3d.get_device();             -- fetch fresh each call (never hold a stale device ptr)
    local p = ffi.new('IDirect3DTexture8*[1]');
    if (C.D3DXCreateTextureFromFileA(device, addon.path .. 'assets/' .. name, p) ~= C.S_OK) then return 0; end
    local t = d3d.gc_safe_release(ffi.cast('IDirect3DBaseTexture8*', p[0]));
    refs[#refs+1] = t;
    return tonumber(ffi.cast('uintptr_t', ffi.cast('void*', t))) or 0;
end
local function ensure_tex()
    if (circle_ptr ~= 0 and spark_ptr ~= 0 and smoke_ptr ~= 0 and font_ptr ~= 0) then return; end
    if (tex_attempts >= 120) then return; end    -- give up after ~2s; don't hammer a missing file every frame
    tex_attempts = tex_attempts + 1;
    if (circle_ptr == 0) then circle_ptr = load_tex('circle.png');     end
    if (spark_ptr  == 0) then spark_ptr  = load_tex('spark.png');      end
    if (smoke_ptr  == 0) then smoke_ptr  = load_tex('smoke_soft.png'); end
    if (font_ptr   == 0) then font_ptr   = load_tex('font.png');       end
    if (tex_attempts == 120 and (circle_ptr == 0 or spark_ptr == 0 or smoke_ptr == 0 or font_ptr == 0)) then
        print(chat.header(addon.name) .. chat.error('could not load some textures from assets/ - effects may not render.'));
    end
end

-- Math + packing helpers (vertex = X=east, V=vertical, N=north; UP = SMALLER V)
local function argb(c, a)
    if (c == nil) then c = { 1, 1, 1 }; end
    a = math.floor(math.max(0, math.min(255, a or 255)));   -- clamp so a high alpha can't overflow the u32
    return a * 0x1000000
         + math.floor(math.max(0, math.min(1, c[1]))*255+0.5)*0x10000
         + math.floor(math.max(0, math.min(1, c[2]))*255+0.5)*0x100
         + math.floor(math.max(0, math.min(1, c[3]))*255+0.5);
end
local function mix3(a, b, f) return { a[1]+(b[1]-a[1])*f, a[2]+(b[2]-a[2])*f, a[3]+(b[3]-a[3])*f }; end
-- Marshal the packed buffer to occlude3d via the 3-arg RaiseEvent(name, ptr, size) form: ffi.copy is a
-- single memcpy into a reusable buffer.
-- The plugin reads eventData as raw bytes (memcpy) with a size bounds-check, so it's byte-identical.
local RAISE_CAP  = 16384;                                  -- occlude3d's per-owner buffer cap
local raise_buf  = ffi.new('uint8_t[?]', RAISE_CAP);
local raise_ptr  = tonumber(ffi.cast('uintptr_t', raise_buf));
local plugin_mgr = AshitaCore:GetPluginManager();
local function raise(data)
    local n = #data;
    if (n == 0 or n > RAISE_CAP) then return; end          -- never overflow the scratch buffer
    ffi.copy(raise_buf, data, n);
    plugin_mgr:RaiseEvent('occlude3d', raise_ptr, n);
end
local function submit(owner, ttl, items)
    if (#items == 0) then return; end
    raise(struct.pack('<IIII', MAGIC, owner, ttl, #items) .. table.concat(items));
end
local function clear_owner(owner) raise(struct.pack('<IIII', MAGIC, owner, 0, 0)); end
local function h01(i, salt)
    local x = math.sin(i * 12.9898 + salt * 78.233 + 0.5) * 43758.5453;
    return x - math.floor(x);
end

local function disc(tex, px, pv, pn, R, ang, color, typeword)        -- flat textured ground quad (TEXTRIS)
    local cs, sn = math.cos(ang), math.sin(ang);
    local function v(e, n, u, vv)
        local er, nr = e*cs - n*sn, e*sn + n*cs;
        return struct.pack('<fffIff', px + er, pv, pn + nr, color, u, vv);
    end
    local c0, c1, c2, c3 = v(-R,-R,0,0), v(R,-R,1,0), v(R,R,1,1), v(-R,R,0,1);
    return struct.pack('<III', typeword, tex, 6) .. c0 .. c1 .. c2 .. c0 .. c2 .. c3;
end
local function particle(tex, px, pv, pn, hw, hh, vx, vy, vz, fade, color, flag)  -- type 15: submit-once drift+fade
    return struct.pack('<IIIfffffffff', 15 + (flag or 1)*256, tex or 0, color, px, pv, pn, hw, hh, vx, vy, vz, fade or 0.5);
end
local function text_item(str, px, pv, pn, sz, color, align, outline_lv)          -- type 6: billboarded atlas text
    local tw = 6;
    if (outline_lv and outline_lv > 0) then
        local lv = math.max(1, math.min(15, math.floor(outline_lv)));
        tw = tw + 0x10000 + lv * 0x20000;
    end
    return struct.pack('<IIIffffII', tw, FONT_ID, color, sz, px, pv, pn, align or 0, #str) .. str;
end

local function player_pos()
    local mm  = AshitaCore:GetMemoryManager();
    local idx = mm:GetParty():GetMemberTargetIndex(0);
    if (idx == 0) then return nil; end
    local e = mm:GetEntity();
    local x, v, n = e:GetLocalPositionX(idx), e:GetLocalPositionZ(idx), e:GetLocalPositionY(idx);
    if (x == 0 and n == 0) then return nil; end
    return { x, v, n };
end
local function ensure_font()
    if (font_registered or font_ptr == 0) then return; end
    raise(struct.pack('<IIIIIIf', O3DF_MAGIC, FONT_ID, font_ptr, FONT_COLS, FONT_ROWS, FONT_FIRST, FONT_ADV));
    font_registered = true;
end

-- RUNE CIRCLE: a flat seal at your feet + the 8 compass directions floating around it
-- {east, north, label} -- N = +north, E = +east, etc.
local DIRS = {
    {0,1,'N'}, {0.7071,0.7071,'NE'}, {1,0,'E'}, {0.7071,-0.7071,'SE'},
    {0,-1,'S'}, {-0.7071,-0.7071,'SW'}, {-1,0,'W'}, {-0.7071,0.7071,'NW'},
};
local function submit_rune(t)
    local p = player_pos(); if (p == nil) then return; end
    local px, pv, pn = p[1], p[2], p[3];
    local feet = pv - 0.03;
    local base = W.rune_col;   -- glow + glyph colors are lighter tints of the one picked color
    local TH = { base = base, glow = mix3(base, {1,1,1}, 0.20), rune = mix3(base, {1,1,1}, 0.55) };
    local spin = W.rune_spin[1];
    local s16 = math.sin(t*1.6);
    local breath = 0.78 + 0.22*(0.5+0.5*s16);
    local R = W.rune_radius[1]*(1.0 + 0.04*s16);
    local angA, angB = t*spin, -t*spin*1.7;
    local items = {};

    -- flat seal: base ring (CW) + inner counter-ring (CCW) + an additive ring & hot core
    items[#items+1] = disc(circle_ptr, px, feet, pn, R,     angA, argb(TH.base, math.floor(200*breath)), TEX_TRIS);
    items[#items+1] = disc(circle_ptr, px, feet, pn, R*0.6, angB, argb(TH.base, math.floor(210*breath)), TEX_TRIS);
    items[#items+1] = disc(circle_ptr, px, feet-0.010, pn, R*0.92, angA*0.4, argb(TH.glow, math.floor(70*breath)),  TEX_TRIS_ADD);
    items[#items+1] = disc(circle_ptr, px, feet-0.015, pn, R*0.30, t*0.9,    argb(TH.glow, math.floor(120*breath)), TEX_TRIS_ADD);

    -- the 8 compass directions, floating + gently bobbing at fixed positions
    if (W.rune_dirs[1]) then
        local dir_col = argb(TH.rune, math.floor(235*breath));   -- same for all 8 labels; hoist out of loop
        for i = 1, #DIRS do
            local d = DIRS[i];
            local bob = feet - 0.22 - 0.06*math.sin(t*2.0 + i);
            items[#items+1] = text_item(d[3], px + d[1]*R*1.15, bob, pn + d[2]*R*1.15, 0.5, dir_col, 0, 6);
        end
    end

    submit(OWNER_RUNE, 250, items);
end

-- EXPLOSION: a one-shot burst - flash + a fast spark shockwave (sphere) +
-- slower debris/embers + rising smoke + an expanding ground ring. All submit-once
-- PARTICLEs (plugin animates drift+fade); the ground ring is animated per-frame.
local boom_ring_t0 = nil;
local function fire_boom(p)
    if (p == nil) then return; end
    local cx, cy, cn = p[1], p[2]-W.boom_height[1], p[3];   -- burst center (height above feet)
    local salt = math.floor(os.clock()*1000) % 100000;
    local sz, pw = W.boom_size[1], W.boom_power[1];
    local col = W.boom_col;
    local n  = math.max(20, math.min(280, math.floor(W.boom_count[1])));

    -- flash: a few big white sparks at the center, very short
    local A = {};
    for i = 1, 6 do
        local hw = (0.8 + 0.5*h01(i, salt+30))*sz;
        A[#A+1] = particle(spark_ptr, cx, cy, cn, hw, hw, 0, 0, 0, 0.0, argb({1.0,1.0,0.92}, 255), 0x01);
    end
    submit(OWNER_BMF, 220, A);

    local life = W.boom_life[1];
    -- shockwave: a full 360-degree sphere of fast sparks. 
    -- Hashes use distinct inputs (i*7+k) so direction/speed are INDEPENDENT.
    -- Correlated hashes (salt vs salt+1) were what collapsed it into a spiral.
    local B = {};
    for i = 1, n do
        local u   = h01(i*7+1, salt)*2 - 1;
        local phi = h01(i*7+2, salt) * TwoPi;
        local s   = (6.0 + 5.0*h01(i*7+3, salt))*pw;
        local sxy = math.sqrt(math.max(0, 1-u*u));
        local heat= math.min(1, (s/pw - 6.0)/5.0);
        local c   = mix3(col, {1,1,1}, heat*0.85);
        local hw  = (0.10 + 0.10*h01(i*7+4, salt))*sz;
        B[#B+1] = particle(spark_ptr, cx, cy, cn, hw, hw, sxy*math.cos(phi)*s, u*s, sxy*math.sin(phi)*s, 0.12, argb(c, 255), 0x01);
    end
    submit(OWNER_BMA, math.floor(1100*life), B);

    -- debris/embers: slower, warmer, also a full 360 sphere (own salt base = different scatter)
    local D, nd = {}, math.floor(n*0.35);
    for i = 1, nd do
        local u   = h01(i*7+1, salt+5)*2 - 1;
        local phi = h01(i*7+2, salt+5) * TwoPi;
        local s   = (1.8 + 2.6*h01(i*7+3, salt+5))*pw;
        local sxy = math.sqrt(math.max(0, 1-u*u));
        local hw  = (0.08 + 0.08*h01(i*7+4, salt+5))*sz;
        D[#D+1] = particle(spark_ptr, cx, cy, cn, hw, hw, sxy*math.cos(phi)*s, u*s, sxy*math.sin(phi)*s, 0.25,
            argb(mix3(col, {1.0,0.30,0.10}, 0.4), 220), 0x01);
    end
    submit(OWNER_BMB, math.floor(2200*life), D);

    -- smoke: rises from LOW so it pools near the ground, not the body
    if (W.boom_smoke[1]) then
        local smk_y = p[2] - W.boom_smoke_h[1];
        local E, ne = {}, math.floor(n*0.18);
        for i = 1, ne do
            local a   = h01(i*7+1, salt+9) * TwoPi;
            local out = (0.4 + 0.9*h01(i*7+2, salt+9))*pw;
            local hw  = (0.5 + 0.8*h01(i*7+3, salt+9))*sz;
            E[#E+1] = particle(smoke_ptr, cx, smk_y, cn, hw, hw, math.cos(a)*out, -(0.4+0.4*h01(i*7+4, salt+9)), math.sin(a)*out, 0.15,
                argb({0.17,0.16,0.16}, 120), 0x00);
        end
        submit(OWNER_BMC, math.floor(3000*life), E);
    else
        clear_owner(OWNER_BMC);
    end

    boom_ring_t0 = os.clock();                            -- kick the per-frame ground ring
end
local function submit_boom_ring(t)
    if (boom_ring_t0 == nil) then return; end
    local age = t - boom_ring_t0;
    if (age > 0.5) then boom_ring_t0 = nil; clear_owner(OWNER_RING); return; end
    local p = player_pos(); if (p == nil) then return; end
    local f  = age/0.5;
    local rr = f * W.boom_size[1] * 4.0;                  -- expanding shock ring on the ground
    submit(OWNER_RING, 100, { disc(circle_ptr, p[1], p[2]-0.05, p[3], rr, 0, argb(W.boom_col, math.floor(200*(1-f))), TEX_TRIS_ADD) });
end

-- UI
local colors = {
    sub       = { 1.0, 0.65, 0.26, 1.0 },
    enabled   = { 0.4, 1.0,  0.4,  1.0 },
    disabled  = { 1.0, 0.4,  0.4,  1.0 },
    dimmed    = { 0.5, 0.5,  0.5,  1.0 },
    cat_sel   = { 0.20, 0.45, 0.20, 1.0 },
    cat_hover = { 0.25, 0.50, 0.25, 1.0 },
};
local function sub_header(label)
    imgui.TextColored(colors.sub, label);
    imgui.Separator();
end
local function tip(text)
    if (imgui.IsItemHovered()) then
        imgui.BeginTooltip();
        imgui.PushTextWrapPos(300);
        imgui.Text(text);
        imgui.PopTextWrapPos();
        imgui.EndTooltip();
    end
end
local ITEM_W = 160;
local function sf(l, k, mn, mx, fmt) imgui.SetNextItemWidth(ITEM_W); if (imgui.SliderFloat(l, W[k], mn, mx, fmt or '%.2f')) then dirty = true; end end
local function si(l, k, mn, mx)      imgui.SetNextItemWidth(ITEM_W); if (imgui.SliderInt(l, W[k], mn, mx))                then dirty = true; end end
local function cb(l, k)              if (imgui.Checkbox(l, W[k]))                          then dirty = true; end end
local function cl(l, k)              imgui.SetNextItemWidth(ITEM_W); if (imgui.ColorEdit3(l, W[k]))                        then dirty = true; end end

local CATEGORIES   = { 'Rune Circle', 'Explosions' };
local selected_cat = 1;
local SIDEBAR_W    = 116;

local function render_cat_rune()
    sub_header('Rune Circle');
    cb('Show##rune', 'rune_on');                tip('Draw the rune circle at your feet.');
    imgui.Spacing();
    sf('Radius##rune', 'rune_radius', 1.0, 6.0, '%.1f yalms'); tip('Overall size of the seal.');
    sf('Spin##rune',   'rune_spin',   0.0, 1.0);              tip('Rotation speed of the seal rings.');
    cl('Color', 'rune_col');                    tip('Seal color; the glow and floating glyphs are lighter tints of it.');
    cb('Floating directions', 'rune_dirs');     tip('Show the 8 compass directions (N / NE / E ...) floating around the circle.');
end

local function render_cat_boom()
    sub_header('Explosions');
    cb('Auto-repeat', 'boom_on');               tip('Continuously fire explosions on the interval below.');
    imgui.SameLine();
    if (imgui.Button('Boom now')) then fire_boom(player_pos()); boom_ring_t0 = os.clock(); end
    tip('Fire a single explosion right now (same as /mc boom).');
    imgui.Spacing();

    sub_header('Tuning');
    sf('Interval (s)',  'boom_interval', 0.5, 8.0, '%.1f s'); tip('Seconds between auto-repeat explosions.');
    si('Count',         'boom_count', 20, 280);              tip('Spark particles per burst.');
    sf('Size',          'boom_size',  0.4, 2.5);             tip('Particle and shock-ring size.');
    sf('Power (blast)', 'boom_power', 0.4, 2.5);             tip('How fast / far the sparks fly out.');
    sf('Lifetime',      'boom_life',  0.4, 2.5);             tip('How long the sparks, debris and smoke last.');
    sf('Burst height',  'boom_height', 0.0, 2.5, '%.1f yalms'); tip('Height above your feet where the sparks emanate.');
    imgui.Spacing();

    sub_header('Smoke');
    cb('Smoke',        'boom_smoke');            tip('Leave rising smoke behind after the blast.');
    sf('Smoke height', 'boom_smoke_h', 0.0, 2.5, '%.1f yalms'); tip('Height the smoke rises from (lower = pools at the ground).');
    imgui.Spacing();

    sub_header('Color');
    cl('Spark color', 'boom_col');              tip('Color the sparks cool to, and the shock-ring tint.');
end

local cat_renderers = { render_cat_rune, render_cat_boom };

local function render_ui()
    imgui.SetNextWindowSize({ 470, 380 }, ImGuiCond_FirstUseEver);
    if (not imgui.Begin('Magic Circle v' .. addon.version .. '##magiccircle', show, ImGuiWindowFlags_None)) then
        imgui.End(); return;
    end

    cb('Enabled', 'enabled'); tip('Master on/off for all Magic Circle effects.');
    imgui.SameLine();
    if (occ3d_ok) then imgui.TextColored(colors.enabled, 'occlude3d: loaded');
    else               imgui.TextColored(colors.disabled, 'occlude3d: NOT loaded  (/load occlude3d)'); end
    imgui.Spacing(); imgui.Separator(); imgui.Spacing();

    local footer_h = imgui.GetFrameHeightWithSpacing() + imgui.GetStyle().ItemSpacing.y + 4;

    imgui.BeginChild('##sidebar', { SIDEBAR_W, -footer_h }, ImGuiChildFlags_Borders);
    imgui.PushStyleColor(ImGuiCol_Header,        colors.cat_sel);
    imgui.PushStyleColor(ImGuiCol_HeaderHovered, colors.cat_hover);
    imgui.PushStyleColor(ImGuiCol_HeaderActive,  colors.cat_sel);
    for ci = 1, #CATEGORIES do
        if (imgui.Selectable(CATEGORIES[ci] .. '##cat' .. tostring(ci), ci == selected_cat)) then
            selected_cat = ci;
        end
    end
    imgui.PopStyleColor(3);
    imgui.EndChild();

    imgui.SameLine();
    imgui.BeginChild('##detail', { 0, -footer_h }, ImGuiChildFlags_Borders);
    cat_renderers[selected_cat]();
    imgui.EndChild();

    imgui.Separator();
    imgui.TextColored(colors.dimmed, '/mc  -  /mc boom');
    local reset_w  = 150;
    local avail_w  = imgui.GetContentRegionAvail();
    imgui.SameLine(avail_w - reset_w);
    if (imgui.Button('Reset to Defaults', { reset_w, 0 })) then
        for _, k in ipairs(SCALARS) do W[k] = { default_settings[k] }; end
        for _, k in ipairs(COLORS)  do W[k] = { default_settings[k][1], default_settings[k][2], default_settings[k][3] }; end
        dirty = true;
    end
    tip('Reset all Magic Circle settings to their defaults.');

    imgui.End();
end

-- Events
local prev_rune  = false;
local boom_next  = 0;
local draw_warned = false;
-- All geometry submission lives here so it can be pcall-isolated: a runtime error (e.g. from a corrupt
-- setting) drops one frame instead of throwing out of the present hook and spamming the Ashita log.
local function present_draw(t, present)
    local rune = present and W.rune_on[1];
    if (rune) then submit_rune(t); elseif (prev_rune) then clear_owner(OWNER_RUNE); end
    prev_rune = rune;

    if (present and W.boom_on[1] and t >= boom_next) then
        boom_next = t + math.max(0.5, W.boom_interval[1]);
        fire_boom(player_pos());
    end
    if (present) then submit_boom_ring(t); end
end
ashita.events.register('d3d_present', 'mc_present', function()
    ensure_tex();
    local present = W.enabled[1] and occ3d_present();
    if (present) then ensure_font(); end
    local t = os.clock();

    local ok, err = pcall(present_draw, t, present);   -- args, not a closure (no per-frame alloc)
    if (not ok and not draw_warned) then
        draw_warned = true;
        print(chat.header(addon.name) .. chat.error('effect error (further warnings suppressed): ' .. tostring(err)));
    end

    if (show[1]) then render_ui(); end
    if (dirty) then sync_save(); dirty = false; end
end);

ashita.events.register('command', 'mc_command', function(e)
    local args = e.command:args();
    if (#args == 0 or not args[1]:lower():any('/mc', '/magiccircle')) then return; end
    e.blocked = true;
    local sub = (#args >= 2) and args[2]:lower() or nil;
    if (sub == 'boom') then
        fire_boom(player_pos()); boom_ring_t0 = os.clock();
    else
        show[1] = not show[1];
    end
end);

ashita.events.register('unload', 'mc_unload', function()
    pcall(settings.save);
    if (not occ3d_present()) then return; end
    for _, ow in ipairs(ALL_OWNERS) do clear_owner(ow); end
    raise(struct.pack('<IIIIIIf', O3DF_MAGIC, FONT_ID, 0, 0, 0, 0, 0.0));
end);
