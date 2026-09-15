-- #########################################################
-- Astro Realm bot lifecycle (AiPlayerbot.Lifecycle.* / AiPlayerbot.Arrivals.*)
-- The worldserver also creates these tables on start when the lifecycle is enabled.
--
-- How a website tells bots from real players:
--   a character is a bot  <=> its guid is in acore_playerbots.astro_bot_persona
--   an account is a bot   <=> its id is account_id in astro_bot_persona
--   (bot accounts are also named <AiPlayerbot.RandomBotAccountPrefix><n>, default RNDBOT<n>)
-- Everything else about a bot is a normal account / character row:
--   acore_auth.account.joindate      = the moment the bot signed up (arrived)
--   acore_characters.characters.totaltime / level / money grow by playing
--
-- Example:
--   SELECT c.guid, c.name, c.level, c.totaltime, p.status, p.playstyle, p.chronotype,
--          FROM_UNIXTIME(p.arrived_at) AS arrived, FROM_UNIXTIME(NULLIF(p.break_until, 0)) AS back_at
--   FROM acore_characters.characters c
--   JOIN acore_playerbots.astro_bot_persona p ON p.guid = c.guid;
-- #########################################################

-- One row per bot character (arrivals create one account with one character)
CREATE TABLE IF NOT EXISTS `astro_bot_persona` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'acore_characters.characters.guid',
    `account_id` INT UNSIGNED NOT NULL COMMENT 'acore_auth.account.id',
    `arrived_at` INT UNSIGNED NOT NULL COMMENT 'unix time the bot signed up',
    `race` TINYINT UNSIGNED NOT NULL,
    `class` TINYINT UNSIGNED NOT NULL,
    `chronotype` VARCHAR(16) NOT NULL COMMENT 'daytime (peak ~11h) | evening (~20h) | night_owl (~23:30) | any (Europe/Oslo)',
    `sessions_per_week` TINYINT UNSIGNED NOT NULL COMMENT 'typical play sessions per week',
    `session_minutes` SMALLINT UNSIGNED NOT NULL COMMENT 'typical session length; each session varies around it',
    `playstyle` VARCHAR(16) NOT NULL COMMENT 'dominant style: quester | grinder | crafter | explorer | social',
    `style_quester` TINYINT UNSIGNED NOT NULL COMMENT 'style weights in percent, the five add up to 100',
    `style_grinder` TINYINT UNSIGNED NOT NULL,
    `style_crafter` TINYINT UNSIGNED NOT NULL,
    `style_explorer` TINYINT UNSIGNED NOT NULL,
    `style_social` TINYINT UNSIGNED NOT NULL,
    `break_factor` FLOAT NOT NULL DEFAULT 1 COMMENT 'personal multiplier on break (and quit) rates',
    `status` VARCHAR(16) NOT NULL DEFAULT 'active' COMMENT 'active | break | quit (quit bots never log in again)',
    `break_until` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time the current/last break ends',
    `quit_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time the bot quit, 0 = not quit',
    `next_login_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'next planned login, 0 = playing now or quit',
    `last_login_at` INT UNSIGNED NOT NULL DEFAULT 0,
    `last_logout_at` INT UNSIGNED NOT NULL DEFAULT 0,
    `sessions_played` INT UNSIGNED NOT NULL DEFAULT 0,
    `breaks_taken` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`),
    UNIQUE KEY `idx_account` (`account_id`),
    KEY `idx_status` (`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Astro Realm: persona and lifecycle of each random bot';

-- Planned sign-ups; rows are written once per week (status pending) and completed at runtime
CREATE TABLE IF NOT EXISTS `astro_bot_arrivals` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `week_start` INT UNSIGNED NOT NULL COMMENT 'unix time of Monday 00:00 local time of the planned week',
    `scheduled_at` INT UNSIGNED NOT NULL,
    `status` VARCHAR(16) NOT NULL DEFAULT 'pending' COMMENT 'pending | creating | done | failed | capped | empty (week marker)',
    `account_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `created_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_status_time` (`status`, `scheduled_at`),
    KEY `idx_week` (`week_start`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Astro Realm: planned and completed bot arrivals';
