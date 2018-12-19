.PHONY: tests

build:
	$(MAKE) -C c

## submodules:
vendor:
	git submodule update --init --recursive --force

## tests commands:
tests:
	pytest --ignore=vendor

## style commands:
style: ## run code style check on application sources and tests
	flake8 python/ tests/
	isort --check-only --recursive python/ tests/
	black python/ tests/ --check

isort:
	isort -y --recursive python/ tests/

black:
	black python/ tests/
