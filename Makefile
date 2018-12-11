## style commands:

style: ## run code style check on application sources and tests
	flake8 python/ tests/
	isort --check-only
	black python/ tests/ --check

isort:
	isort -y --recursive python/ tests/

black:
	black python/ tests/