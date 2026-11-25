#!/usr/bin/env python3
"""Print the API's OpenAPI document (no database needed). Used to generate the dashboard's typed
client: `npm run gen:api` in frontend/."""

import json
import sys

from finforge.server.api import create_app
from finforge.server.settings import Settings

if __name__ == "__main__":
    app = create_app(Settings(database_url="postgresql+psycopg://unused@localhost/unused"))
    json.dump(app.openapi(), sys.stdout, indent=1, sort_keys=True)
    sys.stdout.write("\n")
