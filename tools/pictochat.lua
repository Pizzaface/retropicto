-- pictochat.lua — Wireshark dissector for Nintendo DS local wireless / PictoChat.
--
-- Install: copy into your Wireshark "Personal Lua Plugins" folder
--   (Help > About Wireshark > Folders), then Analyze > Reload Lua Plugins.
--
-- What it does today:
--   * Flags any 802.11 frame whose source/dest/BSSID is a Nintendo OUI.
--   * Classifies it as Beacon / Data / other.
--   * For beacons, finds Nintendo's vendor-specific IE (tag 221, OUI 00:09:BF)
--     and breaks out the DS "beacon platform data" fields that are known.
--   * For data frames, surfaces the Nintendo payload as a labelled scaffold so
--     you can annotate fields as you reverse them.
--
-- HONESTY NOTE: the 802.11 framing and the beacon vendor IE are well documented.
-- The PictoChat *application* payload (chat room state, the drawn message
-- bitmaps, keyboard glyphs) is only partially reverse-engineered publicly. The
-- data-frame section below is deliberately a scaffold with the byte offsets
-- exposed — treat its field guesses as hypotheses to confirm, not fact.

local p_nds = Proto("pictochat", "Nintendo DS Local Wireless / PictoChat")

-- Fields this dissector adds to the tree.
local f_is_nds   = ProtoField.bool  ("pictochat.is_nds",   "Nintendo DS frame")
local f_role     = ProtoField.string("pictochat.role",     "Frame role")
local f_host_mac = ProtoField.ether ("pictochat.host",     "DS host (BSSID)")
-- Beacon vendor IE
local f_vendor   = ProtoField.bytes ("pictochat.vendor_ie","Nintendo vendor IE")
local f_game_oui = ProtoField.uint24("pictochat.game_oui", "Vendor OUI", base.HEX)
-- Data-frame scaffold
local f_payload  = ProtoField.bytes ("pictochat.payload",  "DS payload")
local f_p_len    = ProtoField.uint16("pictochat.payload_len", "DS payload length")

p_nds.fields = {
    f_is_nds, f_role, f_host_mac, f_vendor, f_game_oui, f_payload, f_p_len,
}

-- Extractors from the built-in 802.11 dissector (run before postdissectors).
local e_sa       = Field.new("wlan.sa")
local e_da       = Field.new("wlan.da")
local e_bssid    = Field.new("wlan.bssid")
local e_typesub  = Field.new("wlan.fc.type_subtype")

-- Nintendo OUIs (first 3 MAC bytes). Mirror of main/nintendo.h.
local NINTENDO_OUI = {
    ["00:09:bf"]=true, ["00:16:56"]=true, ["00:17:ab"]=true, ["00:19:1d"]=true,
    ["00:1a:e9"]=true, ["00:1b:7a"]=true, ["00:1b:ea"]=true, ["00:1c:be"]=true,
    ["00:1d:bc"]=true, ["00:1e:35"]=true, ["00:1f:32"]=true, ["00:21:47"]=true,
    ["00:21:bd"]=true, ["00:22:4c"]=true, ["00:22:aa"]=true, ["00:22:d7"]=true,
    ["00:23:31"]=true, ["00:23:cc"]=true, ["00:24:1e"]=true, ["00:24:44"]=true,
    ["00:24:f3"]=true, ["00:25:a0"]=true, ["00:26:59"]=true, ["00:27:09"]=true,
    ["04:03:d6"]=true, ["e8:4e:ce"]=true,
}

local function oui_of(ether_field)
    if not ether_field then return nil end
    local s = tostring(ether_field)          -- "00:09:bf:12:34:56"
    return s:sub(1, 8):lower()
end

local function is_nintendo(ether_field)
    local oui = oui_of(ether_field)
    return oui ~= nil and NINTENDO_OUI[oui] == true
end

-- 802.11 type/subtype values we care about.
local SUBTYPE_BEACON = 0x08  -- mgmt(0) subtype 8
local TYPE_DATA      = 0x2

-- Locate Nintendo's vendor-specific IE (tag 221 / 0xDD) inside a beacon body.
-- Beacon body begins at MAC header (24) + fixed params (12) = offset 36.
-- Returns (offset, length) of the IE *contents* after the tag/len bytes, or nil.
local function find_vendor_ie(tvb)
    local pos = 36
    local n = tvb:len()
    while pos + 2 <= n do
        local tag = tvb(pos, 1):uint()
        local len = tvb(pos + 1, 1):uint()
        if pos + 2 + len > n then break end
        if tag == 221 and len >= 3 then
            local oui = tvb(pos + 2, 3):bytes():tohex()
            if oui:lower() == "0009bf" then
                return pos + 2, len
            end
        end
        pos = pos + 2 + len
    end
    return nil
end

function p_nds.dissector(tvb, pinfo, tree)
    local sa, da, bssid = e_sa(), e_da(), e_bssid()
    if not (is_nintendo(sa) or is_nintendo(da) or is_nintendo(bssid)) then
        return
    end

    local st = tree:add(p_nds, tvb(), "Nintendo DS Local Wireless / PictoChat")
    st:add(f_is_nds, true):set_generated()
    if bssid then st:add(f_host_mac, bssid.range):set_generated() end

    local ts = e_typesub()
    local tsv = ts and ts.value or -1
    local role = "Nintendo DS frame"
    if tsv == SUBTYPE_BEACON then
        role = "Host beacon (advertising a PictoChat/local session)"
    elseif ts and math.floor(tsv / 16) == TYPE_DATA then
        role = "Data (PictoChat payload)"
    end
    st:add(f_role, role):set_generated()
    pinfo.cols.protocol:set("PictoChat")
    pinfo.cols.info:prepend("[DS] ")

    -- --- Beacon: decode the Nintendo vendor IE ---
    if tsv == SUBTYPE_BEACON then
        local off, len = find_vendor_ie(tvb)
        if off then
            local vt = st:add(f_vendor, tvb(off, len))
            vt:add(f_game_oui, tvb(off, 3))
            -- Bytes after the OUI carry Nintendo's beacon platform data:
            -- game/title id, session/host state, player counts. Exact layout
            -- varies by title; PictoChat is a system app with its own values.
            -- Exposed here as raw bytes for you to map as you confirm them.
            if len > 3 then
                vt:add(tvb(off + 3, len - 3),
                       "Beacon platform data (title id / session state — raw)")
            end
        else
            st:add(tvb(), "No Nintendo vendor IE found in this beacon")
        end
        return
    end

    -- --- Data frame: expose the DS payload as a scaffold ---
    -- Compute the 802.11 header length to find where the payload starts.
    local fc1 = tvb(1, 1):uint()
    local to_ds   = (fc1 % 2) == 1
    local from_ds = (math.floor(fc1 / 2) % 2) == 1
    local hdr = 24
    if to_ds and from_ds then hdr = hdr + 6 end        -- addr4
    local subtype = math.floor(tsv % 16)
    if subtype >= 8 then hdr = hdr + 2 end             -- QoS control

    if tvb:len() > hdr then
        local plen = tvb:len() - hdr
        local pt = st:add(f_payload, tvb(hdr, plen))
        pt:add(f_p_len, plen):set_generated()
        -- SCAFFOLD: the first bytes of a DS data payload are Nintendo's own
        -- transport framing (sequencing/host-client multiplexing) ahead of the
        -- PictoChat message data. Confirm these offsets against live captures
        -- before trusting them.
        if plen >= 2 then
            pt:add(tvb(hdr, 2), "Transport header (hypothesis — verify)")
        end
        if plen > 2 then
            pt:add(tvb(hdr + 2, plen - 2),
                   "PictoChat message data (room text / drawing bitmap — raw)")
        end
    end
end

register_postdissector(p_nds)
