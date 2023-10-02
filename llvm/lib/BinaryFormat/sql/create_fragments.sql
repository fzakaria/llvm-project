R"(
CREATE TABLE IF NOT EXISTS Fragment (
    address BLOB PRIMARY KEY,
    type VARCHAR,
    layoutOrder INTEGER,
    offset INTEGER,
    hasInstructions INTEGER,
    bundlePadding INTEGER,
    contents BLOB
);)"