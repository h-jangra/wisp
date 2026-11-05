local widle = widle

local SECOND = 1000
local W, H = 200, 56

function on_timer_ms()
	return SECOND
end

function on_timer()
	widle.redraw()
end

function on_draw()
	widle.rect(0, 0, W, H, 0xDD202020)
	widle.rrect(4, 4, W-8, H-8, 6, 0xDD3050A0)
	widle.rrect(12, 12, W-24, H-24, 3, 0xDD202020)

	local t = os.date("!%H:%M:%S")
	widle.text(t, 16, H/2 + 8, 0xFFFFFFFF)

	local d = os.date("!%a %d %b")
	widle.text(d, 16, H/2 - 14, 0xAAAAAAFF)
end

return {width = W, height = H, anchor = "top right", visible = true, on_timer_ms = SECOND}