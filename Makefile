# ESP32-Garage-Fan - Makefile
# Simple entry points for common operations

.PHONY: help build flash monitor deploy test test-native test-python web lint clean

# Default target
.DEFAULT_GOAL := help

# Colors for terminal output
CYAN := \033[0;36m
GREEN := \033[0;32m
YELLOW := \033[1;33m
RED := \033[0;31m
NC := \033[0m # No Color

PIO_DIR := firmware/arduino
FAN_ENV := feather_esp32s2_fan_controller

help: ## Show this help message
	@echo ""
	@echo "$(CYAN)ESP32-Garage-Fan$(NC)"
	@echo "$(CYAN)════════════════$(NC)"
	@echo ""
	@echo "$(GREEN)Usage:$(NC)"
	@echo "  make <target>"
	@echo ""
	@echo "$(GREEN)Targets:$(NC)"
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "  $(CYAN)%-14s$(NC) %s\n", $$1, $$2}'
	@echo ""
	@echo "$(YELLOW)Note:$(NC) builds read WiFi/MQTT credentials from a gitignored .env"
	@echo "      at the repo root. Copy .env.example and fill it in first."
	@echo ""

build: ## Build the fan controller firmware
	@echo "$(CYAN)Building $(FAN_ENV)...$(NC)"
	@pio run -d $(PIO_DIR) -e $(FAN_ENV)
	@echo "$(GREEN)Build complete$(NC)"

flash: ## Build and flash over USB
	@pio run -d $(PIO_DIR) -e $(FAN_ENV) -t upload

monitor: ## Open the serial monitor
	@pio device monitor -d $(PIO_DIR) -e $(FAN_ENV)

deploy: ## Build and deploy over the air (usage: make deploy IP=10.0.0.5)
	@./scripts/deploy.sh $(IP)

test: test-native test-python ## Run all tests

test-native: ## Run host-side Unity tests
	@echo "$(CYAN)Running native tests...$(NC)"
	@bash scripts/test-native.sh

test-python: ## Run the pytest suite
	@echo "$(CYAN)Running python tests...$(NC)"
	@pytest -q

web: ## Build the console bundle (needs node; output is committed)
	@echo "$(CYAN)Building web console...$(NC)"
	@npm --prefix web ci --silent
	@npm --prefix web run check

lint: ## Run ruff, black and cpplint
	@echo "$(CYAN)Linting...$(NC)"
	@ruff check .
	@black --check .
	@python -m cpplint $$(find $(PIO_DIR)/src -type f \( -name '*.cpp' -o -name '*.h' \) ! -name 'generated_*.h')

clean: ## Clean build artifacts and caches
	@echo "$(CYAN)Cleaning...$(NC)"
	@rm -rf $(PIO_DIR)/.pio
	@find . -type d -name "__pycache__" -exec rm -rf {} + 2>/dev/null || true
	@rm -rf .pytest_cache
	@echo "$(GREEN)Clean complete$(NC)"
