"""Alembic environment: the URL comes from the Config set up by finforge.server.db.migrate."""

from alembic import context
from sqlalchemy import create_engine

from finforge.server.db import Base

target_metadata = Base.metadata


def run_migrations_online() -> None:
    url = context.config.get_main_option("sqlalchemy.url")
    engine = create_engine(url)
    with engine.connect() as connection:
        context.configure(connection=connection, target_metadata=target_metadata)
        with context.begin_transaction():
            context.run_migrations()
    engine.dispose()


run_migrations_online()
