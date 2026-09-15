-- #########################################################
-- Astro Realm arrivals: one row per planned week (AiPlayerbot.Arrivals.*)
-- The worldserver also creates this table on start when the lifecycle is enabled.
--
-- A week is planned once. `plan` says how:
--   full       planned when the week began: the whole weekly budget
--   mid_week   planned after the week began (first start or downtime): only the time still ahead,
--              budget scaled by `share` when AiPlayerbot.Arrivals.ScaleMidWeek = 1
--   topped_up  the week had rows from an older build (which dropped slots already in the past); rows were
--              added once to reach the scaled budget for the rest of the week
-- Times are unix seconds; week_start is Monday 00:00 in the population clock's local time.
-- #########################################################

CREATE TABLE IF NOT EXISTS `astro_bot_arrival_weeks` (
    `week_start` INT UNSIGNED NOT NULL,
    `planned_at` INT UNSIGNED NOT NULL,
    `plan` VARCHAR(16) NOT NULL COMMENT 'full | mid_week | topped_up',
    `share` FLOAT NOT NULL COMMENT 'expected share of the week''s arrivals still ahead when planned (1 if not scaled)',
    `week_budget` FLOAT NOT NULL COMMENT 'full week budget: PerYear * 7 / 365.25 with +-15% noise',
    `budget` INT UNSIGNED NOT NULL COMMENT 'round(week_budget * share): arrivals wanted for the rest of the week',
    `existing` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'rows of an older plan counted towards budget',
    `added` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'rows inserted by this plan',
    `total` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'arrival rows of the week after planning',
    `per_day` VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'rows per day, Monday..Sunday',
    `first_at` INT UNSIGNED NOT NULL DEFAULT 0,
    `last_at` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`week_start`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Astro Realm: one row per planned arrivals week';
