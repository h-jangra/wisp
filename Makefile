BUILD_DIR = build

all:
	meson setup $(BUILD_DIR) --reconfigure 2>/dev/null || meson setup $(BUILD_DIR)
	meson compile -C $(BUILD_DIR)

run: all
	./$(BUILD_DIR)/wisp

test: all
	./$(BUILD_DIR)/wisp --selftest

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean all

.PHONY: all run test clean rebuild
