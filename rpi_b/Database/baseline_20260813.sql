--
-- PostgreSQL database dump
--

\restrict shWqunO8HLXNaE3KS7Y2TjUXuw2Kbp1RsRnk1nFGw5dnrD9xk0m2mv3pZZeTPJN

-- Dumped from database version 17.10 (Debian 17.10-0+deb13u1)
-- Dumped by pg_dump version 17.10 (Debian 17.10-0+deb13u1)

SET statement_timeout = 0;
SET lock_timeout = 0;
SET idle_in_transaction_session_timeout = 0;
SET transaction_timeout = 0;
SET client_encoding = 'UTF8';
SET standard_conforming_strings = on;
SELECT pg_catalog.set_config('search_path', '', false);
SET check_function_bodies = false;
SET xmloption = content;
SET client_min_messages = warning;
SET row_security = off;

--
-- Name: postgis; Type: EXTENSION; Schema: -; Owner: -
--

CREATE EXTENSION IF NOT EXISTS postgis WITH SCHEMA public;


--
-- Name: EXTENSION postgis; Type: COMMENT; Schema: -; Owner: -
--

COMMENT ON EXTENSION postgis IS 'PostGIS geometry and geography spatial types and functions';


--
-- Name: detections_drop_old(integer); Type: FUNCTION; Schema: public; Owner: -
--

CREATE FUNCTION public.detections_drop_old(retain_days integer DEFAULT 14) RETURNS integer
    LANGUAGE plpgsql
    AS $_$
DECLARE
    part record; dropped int := 0;
    cutoff date := now()::date - retain_days;
BEGIN
    FOR part IN
        SELECT c.relname
        FROM pg_inherits i
        JOIN pg_class c ON c.oid = i.inhrelid
        JOIN pg_class p ON p.oid = i.inhparent
        WHERE p.relname = 'detections'
          AND c.relname ~ '^detections_p\d{8}$'
          AND to_date(substring(c.relname FROM '\d{8}$'), 'YYYYMMDD') < cutoff
    LOOP
        EXECUTE format('DROP TABLE %I', part.relname);
        dropped := dropped + 1;
    END LOOP;
    RETURN dropped;
END $_$;


--
-- Name: detections_ensure_partitions(integer); Type: FUNCTION; Schema: public; Owner: -
--

CREATE FUNCTION public.detections_ensure_partitions(days_ahead integer DEFAULT 7) RETURNS integer
    LANGUAGE plpgsql
    AS $$
DECLARE
    d date; nm text; created int := 0;
BEGIN
    FOR i IN 0..days_ahead LOOP
        d  := now()::date + i;
        nm := 'detections_p' || to_char(d, 'YYYYMMDD');
        IF NOT EXISTS (SELECT 1 FROM pg_class WHERE relname = nm) THEN
            EXECUTE format(
                'CREATE TABLE %I PARTITION OF detections FOR VALUES FROM (%L) TO (%L)',
                nm, d::timestamptz, (d + 1)::timestamptz);
            created := created + 1;
        END IF;
    END LOOP;
    RETURN created;
END $$;


--
-- Name: guardx_fire_maintain(integer); Type: FUNCTION; Schema: public; Owner: -
--

CREATE FUNCTION public.guardx_fire_maintain(p_sensor_reading_days integer DEFAULT 30) RETURNS text
    LANGUAGE plpgsql
    AS $$
DECLARE
    n_reading bigint;
BEGIN
    DELETE FROM sensor_reading
        WHERE received_at < now() - make_interval(days => p_sensor_reading_days);
    GET DIAGNOSTICS n_reading = ROW_COUNT;

    RETURN format('sensor_reading -%s (sensor_value는 CASCADE로 함께 정리됨)',
                  n_reading);
END $$;


--
-- Name: FUNCTION guardx_fire_maintain(p_sensor_reading_days integer); Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON FUNCTION public.guardx_fire_maintain(p_sensor_reading_days integer) IS 'fire_schema.sql 보존 정책. sensor_reading/sensor_value만 대상(30일 기본) — fire_event류 5개 감사·이력 테이블은 의도적으로 삭제 대상에서 제외 (2026-08-10 결정, 이 함수 헤더 주석 참조).';


--
-- Name: guardx_maintain(integer, integer, integer, integer, integer); Type: FUNCTION; Schema: public; Owner: -
--

CREATE FUNCTION public.guardx_maintain(p_detections_days integer DEFAULT 14, p_prediction_days integer DEFAULT 180, p_faces_days integer DEFAULT 30, p_flow_days integer DEFAULT 180, p_occupancy_days integer DEFAULT 365) RETURNS text
    LANGUAGE plpgsql
    AS $$
DECLARE
    n_created int; n_dropped int; n_pred bigint;
    n_face bigint; n_flow bigint; n_occ bigint;
BEGIN
    n_created := detections_ensure_partitions(7);
    n_dropped := detections_drop_old(p_detections_days);
    DELETE FROM congestion_prediction
        WHERE predicted_at < now() - make_interval(days => p_prediction_days);
    GET DIAGNOSTICS n_pred = ROW_COUNT;
    DELETE FROM faces
        WHERE ts < now() - make_interval(days => p_faces_days);
    GET DIAGNOSTICS n_face = ROW_COUNT;
    DELETE FROM line_flow
        WHERE bucket_ts < now() - make_interval(days => p_flow_days);
    GET DIAGNOSTICS n_flow = ROW_COUNT;
    DELETE FROM zone_occupancy
        WHERE bucket_ts < now() - make_interval(days => p_occupancy_days);
    GET DIAGNOSTICS n_occ = ROW_COUNT;
    RETURN format('partitions +%s/-%s, predictions -%s, '
                  'faces -%s, line_flow -%s, zone_occupancy -%s',
                  n_created, n_dropped, n_pred, n_face, n_flow, n_occ);
END $$;


SET default_tablespace = '';

SET default_table_access_method = heap;

--
-- Name: actuator_command; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.actuator_command (
    command_id smallint NOT NULL,
    command_key text NOT NULL,
    actuator text NOT NULL,
    kind text NOT NULL,
    CONSTRAINT actuator_command_kind_check CHECK ((kind = ANY (ARRAY['onoff'::text, 'set'::text, 'both'::text])))
);


--
-- Name: alerts; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.alerts (
    alert_id integer NOT NULL,
    incident_id integer NOT NULL,
    message text NOT NULL,
    broadcast_channel text,
    created_at timestamp with time zone DEFAULT now() NOT NULL
);


--
-- Name: alerts_alert_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.alerts_alert_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: alerts_alert_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.alerts_alert_id_seq OWNED BY public.alerts.alert_id;


--
-- Name: button_event; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.button_event (
    button_event_id bigint NOT NULL,
    zone_id smallint NOT NULL,
    sensor_seq bigint NOT NULL,
    press_count integer NOT NULL,
    occurred_at timestamp with time zone DEFAULT now() NOT NULL
);


--
-- Name: button_event_button_event_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.button_event_button_event_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: button_event_button_event_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.button_event_button_event_id_seq OWNED BY public.button_event.button_event_id;


--
-- Name: camera_credentials; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.camera_credentials (
    camera_id integer NOT NULL,
    cam_user text NOT NULL,
    cam_pass text NOT NULL,
    updated_at timestamp with time zone DEFAULT now() NOT NULL
);


--
-- Name: cameras; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.cameras (
    camera_id integer NOT NULL,
    camera_name text NOT NULL,
    resolution_w integer NOT NULL,
    resolution_h integer NOT NULL,
    status text DEFAULT 'ONLINE'::text NOT NULL
);


--
-- Name: cameras_camera_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.cameras_camera_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: cameras_camera_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.cameras_camera_id_seq OWNED BY public.cameras.camera_id;


--
-- Name: congestion_prediction; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.congestion_prediction (
    prediction_id bigint NOT NULL,
    zone_id integer NOT NULL,
    predicted_at timestamp with time zone DEFAULT now() NOT NULL,
    target_ts timestamp with time zone NOT NULL,
    predicted_count integer NOT NULL,
    model_version text,
    config_version integer,
    p10 real,
    p90 real,
    p_over_capacity real,
    warmup boolean
);


--
-- Name: COLUMN congestion_prediction.config_version; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.congestion_prediction.config_version IS 'v9~v12 형상 epoch 태그 (이력 호환). v13(hw_damped_v1)부터 예측은 epoch 비의존 — NULL.';


--
-- Name: COLUMN congestion_prediction.p10; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.congestion_prediction.p10 IS '예측 하한 (10분위, 명 단위 소수). NULL = 구 폴러 적재분';


--
-- Name: COLUMN congestion_prediction.p90; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.congestion_prediction.p90 IS '예측 상한 (90분위, 명 단위 소수). NULL = 구 폴러 적재분';


--
-- Name: COLUMN congestion_prediction.p_over_capacity; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.congestion_prediction.p_over_capacity IS '용량 초과 확률 0..1. 카메라 -1(불명)은 NULL — 확률 0과 다름';


--
-- Name: COLUMN congestion_prediction.warmup; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.congestion_prediction.warmup IS 'true = 모델 워밍업 중(관측 30개 미만) 예측. 평가·경보에서 제외 대상. NULL = 구 적재분(비-warmup)';


--
-- Name: congestion_prediction_prediction_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.congestion_prediction_prediction_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: congestion_prediction_prediction_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.congestion_prediction_prediction_id_seq OWNED BY public.congestion_prediction.prediction_id;


--
-- Name: detections; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections (
    detection_id bigint NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
)
PARTITION BY RANGE (ts);


--
-- Name: COLUMN detections.category; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.detections.category IS '1=Human, 2=Face, 3=Head (v15)';


--
-- Name: COLUMN detections.parent_id; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.detections.parent_id IS 'Face/Head의 부모(사람) object_id. Human은 NULL';


--
-- Name: detections_detection_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.detections_detection_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: detections_detection_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.detections_detection_id_seq OWNED BY public.detections.detection_id;


--
-- Name: detections_p20260730; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260730 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260731; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260731 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260801; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260801 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260802; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260802 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260803; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260803 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260804; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260804 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260805; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260805 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260806; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260806 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260807; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260807 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260808; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260808 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260809; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260809 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260810; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260810 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260811; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260811 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260812; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260812 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260813; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260813 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260814; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260814 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260815; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260815 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260816; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260816 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260817; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260817 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260818; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260818 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260819; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260819 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: detections_p20260820; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.detections_p20260820 (
    detection_id bigint DEFAULT nextval('public.detections_detection_id_seq'::regclass) NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    category smallint NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    geom public.geometry(Point) NOT NULL,
    ts timestamp with time zone NOT NULL,
    parent_id integer,
    display_id bigint,
    global_id bigint,
    raw_channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point),
    next_channel_hint integer,
    handover_ready boolean,
    prediction_confidence real
);


--
-- Name: endpoints; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.endpoints (
    key text NOT NULL,
    value text NOT NULL,
    updated_at timestamp with time zone DEFAULT now() NOT NULL,
    updated_by text,
    note text,
    CONSTRAINT endpoints_key_check CHECK (((key <> ''::text) AND (key <> ALL (ARRAY['node_id'::text, 'timestamp'::text, 'updated_at'::text])))),
    CONSTRAINT endpoints_value_check CHECK ((value <> ''::text))
);


--
-- Name: TABLE endpoints; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON TABLE public.endpoints IS '노드 간 네트워크 주소. 컴파일 상수를 대체한다. guardx/db/rpib/endpoints 로 retained 발행.';


--
-- Name: COLUMN endpoints.key; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.endpoints.key IS '주소 항목 키. MQTT payload 의 필드명이 그대로 이 값이 된다.';


--
-- Name: faces; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.faces (
    face_id bigint NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    object_id integer NOT NULL,
    likelihood real,
    rect_sx integer,
    rect_sy integer,
    rect_ex integer,
    rect_ey integer,
    image_ref text,
    ts timestamp with time zone NOT NULL
);


--
-- Name: faces_face_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.faces_face_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: faces_face_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.faces_face_id_seq OWNED BY public.faces.face_id;


--
-- Name: fire_event; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.fire_event (
    event_id bigint NOT NULL,
    zone_id smallint NOT NULL,
    event_type text NOT NULL,
    cause_channel_id smallint,
    trigger_seq bigint,
    occurred_at timestamp with time zone DEFAULT now() NOT NULL,
    CONSTRAINT fire_event_event_type_check CHECK ((event_type = ANY (ARRAY['fire_confirmed'::text, 'recovered'::text])))
);


--
-- Name: fire_event_command; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.fire_event_command (
    event_id bigint NOT NULL,
    command_id smallint NOT NULL,
    action text NOT NULL,
    value integer,
    published_seq bigint,
    CONSTRAINT fire_event_command_action_check CHECK ((action = ANY (ARRAY['ON'::text, 'OFF'::text, 'SET'::text, 'OPEN'::text, 'CLOSE'::text, 'STOP'::text])))
);


--
-- Name: fire_event_event_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.fire_event_event_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: fire_event_event_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.fire_event_event_id_seq OWNED BY public.fire_event.event_id;


--
-- Name: fire_threshold; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.fire_threshold (
    threshold_id integer NOT NULL,
    gas_raw_min real NOT NULL,
    gas_raw_max real NOT NULL,
    spark_raw_safe real NOT NULL,
    spark_raw_danger real NOT NULL,
    temp_min_c real NOT NULL,
    temp_max_c real NOT NULL,
    humi_safe_percent real NOT NULL,
    humi_danger_percent real NOT NULL,
    irtemp_min_c real NOT NULL,
    irtemp_max_c real NOT NULL,
    weight_gas real NOT NULL,
    weight_spark real NOT NULL,
    weight_temp real NOT NULL,
    weight_humi real NOT NULL,
    weight_irtemp real NOT NULL,
    fire_score_threshold real NOT NULL,
    n_confirm integer NOT NULL,
    n_recover integer NOT NULL,
    freeze_relax_cycles integer NOT NULL,
    min_valid_weight real NOT NULL,
    override_spark_score real NOT NULL,
    override_irtemp_score real NOT NULL,
    is_active boolean DEFAULT false NOT NULL,
    updated_at timestamp with time zone DEFAULT now() NOT NULL,
    updated_by text,
    CONSTRAINT chk_cycles CHECK (((n_confirm > 0) AND (n_recover > 0) AND (freeze_relax_cycles > 0))),
    CONSTRAINT chk_gas_range CHECK ((gas_raw_min < gas_raw_max)),
    CONSTRAINT chk_humi_range CHECK ((humi_safe_percent > humi_danger_percent)),
    CONSTRAINT chk_irtemp_range CHECK ((irtemp_min_c < irtemp_max_c)),
    CONSTRAINT chk_min_valid_weight CHECK (((min_valid_weight > (0)::double precision) AND (min_valid_weight <= (1)::double precision))),
    CONSTRAINT chk_override_range CHECK ((((override_spark_score >= (0)::double precision) AND (override_spark_score <= (100)::double precision)) AND ((override_irtemp_score >= (0)::double precision) AND (override_irtemp_score <= (100)::double precision)))),
    CONSTRAINT chk_score_range CHECK (((fire_score_threshold >= (0)::double precision) AND (fire_score_threshold <= (100)::double precision))),
    CONSTRAINT chk_spark_range CHECK ((spark_raw_safe > spark_raw_danger)),
    CONSTRAINT chk_temp_range CHECK ((temp_min_c < temp_max_c)),
    CONSTRAINT chk_weight_sum CHECK ((abs((((((weight_gas + weight_spark) + weight_temp) + weight_humi) + weight_irtemp) - (1.0)::double precision)) < (0.001)::double precision))
);


--
-- Name: fire_threshold_threshold_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.fire_threshold_threshold_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: fire_threshold_threshold_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.fire_threshold_threshold_id_seq OWNED BY public.fire_threshold.threshold_id;


--
-- Name: fire_zone; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.fire_zone (
    zone_id smallint NOT NULL,
    zone_name text NOT NULL,
    rpia_node_id text NOT NULL,
    rpic_node_id text NOT NULL
);


--
-- Name: incidents; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.incidents (
    incident_id integer NOT NULL,
    zone_id integer NOT NULL,
    incident_type text NOT NULL,
    source_type text NOT NULL,
    source_id bigint,
    severity text,
    status text DEFAULT 'open'::text NOT NULL,
    snapshot_path text,
    detected_at timestamp with time zone DEFAULT now() NOT NULL
);


--
-- Name: incidents_incident_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.incidents_incident_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: incidents_incident_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.incidents_incident_id_seq OWNED BY public.incidents.incident_id;


--
-- Name: line_flow; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.line_flow (
    rule text NOT NULL,
    action text NOT NULL,
    bucket_ts timestamp with time zone NOT NULL,
    flow_count integer NOT NULL
);


--
-- Name: manual_command; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.manual_command (
    manual_command_id bigint NOT NULL,
    zone_id smallint NOT NULL,
    command_id smallint NOT NULL,
    action text NOT NULL,
    value integer,
    source text NOT NULL,
    published_seq bigint,
    issued_at timestamp with time zone DEFAULT now() NOT NULL,
    CONSTRAINT manual_command_action_check CHECK ((action = ANY (ARRAY['ON'::text, 'OFF'::text, 'SET'::text, 'OPEN'::text, 'CLOSE'::text, 'STOP'::text])))
);


--
-- Name: manual_command_manual_command_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.manual_command_manual_command_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: manual_command_manual_command_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.manual_command_manual_command_id_seq OWNED BY public.manual_command.manual_command_id;


--
-- Name: trajectory_segments; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.trajectory_segments (
    camera_id integer NOT NULL,
    segment_id bigint NOT NULL,
    global_id bigint NOT NULL,
    display_id bigint,
    object_id integer NOT NULL,
    channel integer NOT NULL,
    raw_channel integer NOT NULL,
    zone_id integer,
    start_ms bigint NOT NULL,
    end_ms bigint NOT NULL,
    dwell_ms bigint NOT NULL,
    confidence real NOT NULL,
    is_reliable boolean NOT NULL,
    state text NOT NULL,
    segment_ts timestamp with time zone NOT NULL,
    served_at timestamp with time zone DEFAULT now() NOT NULL
);


--
-- Name: reliable_trajectory_segments; Type: VIEW; Schema: public; Owner: -
--

CREATE VIEW public.reliable_trajectory_segments AS
 SELECT camera_id,
    segment_id,
    global_id,
    display_id,
    object_id,
    channel,
    raw_channel,
    zone_id,
    start_ms,
    end_ms,
    dwell_ms,
    confidence,
    is_reliable,
    state,
    segment_ts,
    served_at
   FROM public.trajectory_segments
  WHERE (is_reliable = true);


--
-- Name: schema_migrations; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.schema_migrations (
    version text NOT NULL,
    applied_at timestamp with time zone DEFAULT now() NOT NULL,
    note text
);


--
-- Name: TABLE schema_migrations; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON TABLE public.schema_migrations IS '스키마 변경 적용 이력. 새 마이그레이션은 파일 끝에서 여기 INSERT 할 것.';


--
-- Name: season_threshold; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.season_threshold (
    season_key text NOT NULL,
    season_name text NOT NULL,
    sort_order smallint NOT NULL,
    gas_raw_min real NOT NULL,
    gas_raw_max real NOT NULL,
    spark_raw_safe real NOT NULL,
    spark_raw_danger real NOT NULL,
    temp_min_c real NOT NULL,
    temp_max_c real NOT NULL,
    humi_safe_percent real NOT NULL,
    humi_danger_percent real NOT NULL,
    irtemp_min_c real NOT NULL,
    irtemp_max_c real NOT NULL,
    weight_gas real NOT NULL,
    weight_spark real NOT NULL,
    weight_temp real NOT NULL,
    weight_humi real NOT NULL,
    weight_irtemp real NOT NULL,
    fire_score_threshold real NOT NULL,
    n_confirm integer NOT NULL,
    n_recover integer NOT NULL,
    freeze_relax_cycles integer NOT NULL,
    min_valid_weight real NOT NULL,
    override_spark_score real NOT NULL,
    override_irtemp_score real NOT NULL,
    updated_at timestamp with time zone DEFAULT now() NOT NULL,
    updated_by text,
    CONSTRAINT chk_season_cycles CHECK (((n_confirm > 0) AND (n_recover > 0) AND (freeze_relax_cycles > 0))),
    CONSTRAINT chk_season_gas_range CHECK ((gas_raw_min < gas_raw_max)),
    CONSTRAINT chk_season_humi_range CHECK ((humi_safe_percent > humi_danger_percent)),
    CONSTRAINT chk_season_irtemp_range CHECK ((irtemp_min_c < irtemp_max_c)),
    CONSTRAINT chk_season_min_valid_weight CHECK (((min_valid_weight > (0)::double precision) AND (min_valid_weight <= (1)::double precision))),
    CONSTRAINT chk_season_override_range CHECK ((((override_spark_score >= (0)::double precision) AND (override_spark_score <= (100)::double precision)) AND ((override_irtemp_score >= (0)::double precision) AND (override_irtemp_score <= (100)::double precision)))),
    CONSTRAINT chk_season_score_range CHECK (((fire_score_threshold >= (0)::double precision) AND (fire_score_threshold <= (100)::double precision))),
    CONSTRAINT chk_season_spark_range CHECK ((spark_raw_safe > spark_raw_danger)),
    CONSTRAINT chk_season_temp_range CHECK ((temp_min_c < temp_max_c)),
    CONSTRAINT chk_season_weight_sum CHECK ((abs((((((weight_gas + weight_spark) + weight_temp) + weight_humi) + weight_irtemp) - (1.0)::double precision)) < (0.001)::double precision)),
    CONSTRAINT season_threshold_season_key_check CHECK ((season_key = ANY (ARRAY['default'::text, 'spring'::text, 'summer'::text, 'autumn'::text, 'winter'::text])))
);


--
-- Name: TABLE season_threshold; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON TABLE public.season_threshold IS '화재 임계 계절 프리셋 카탈로그 (읽기 전용). VMS SETTINGS 가 폼에 채우는 원본. 실제 판정에 쓰이는 값은 fire_threshold 의 is_active 행이다.';


--
-- Name: sensor_channel; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.sensor_channel (
    channel_id smallint NOT NULL,
    channel_key text NOT NULL,
    device text NOT NULL,
    unit text NOT NULL,
    value_kind text NOT NULL,
    raw_min integer,
    raw_max integer,
    CONSTRAINT sensor_channel_value_kind_check CHECK ((value_kind = ANY (ARRAY['raw'::text, 'physical'::text])))
);


--
-- Name: sensor_reading; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.sensor_reading (
    reading_id bigint NOT NULL,
    zone_id smallint NOT NULL,
    sensor_seq bigint NOT NULL,
    composite_score real,
    received_at timestamp with time zone DEFAULT now() NOT NULL
);


--
-- Name: sensor_reading_reading_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.sensor_reading_reading_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: sensor_reading_reading_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.sensor_reading_reading_id_seq OWNED BY public.sensor_reading.reading_id;


--
-- Name: sensor_value; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.sensor_value (
    reading_id bigint NOT NULL,
    channel_id smallint NOT NULL,
    value real,
    is_valid boolean NOT NULL
);


--
-- Name: site_config; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.site_config (
    key text NOT NULL,
    value jsonb NOT NULL,
    updated_at timestamp with time zone DEFAULT now() NOT NULL,
    updated_by text
);


--
-- Name: TABLE site_config; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON TABLE public.site_config IS '전역 설정 (SITE 문구·캘리브레이션 등). 서버는 value 를 해석하지 않는다 — 통짜 보관 + retained 발행만. 허용 키와 payload 상한(16KB)은 vms/docs/DB_LINK_AND_MQTT_MIGRATION.md §3.3 이 정본이다.';


--
-- Name: track_path; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.track_path (
    track_id bigint NOT NULL,
    ts timestamp with time zone NOT NULL,
    geom public.geometry(Point) NOT NULL,
    zone_id integer,
    display_id bigint,
    camera_id integer,
    channel integer,
    state text,
    direction text,
    speed real,
    predicted_geom public.geometry(Point)
);


--
-- Name: tracks; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.tracks (
    track_id bigint NOT NULL,
    object_id integer NOT NULL,
    first_seen_at timestamp with time zone NOT NULL,
    last_seen_at timestamp with time zone NOT NULL,
    is_suspect boolean DEFAULT false NOT NULL,
    risk_level text,
    attributes jsonb,
    display_id bigint,
    global_id bigint,
    camera_id integer,
    channel integer,
    state text DEFAULT 'active'::text,
    last_object_id integer
);


--
-- Name: tracks_track_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.tracks_track_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: tracks_track_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.tracks_track_id_seq OWNED BY public.tracks.track_id;


--
-- Name: trajectory_zone_dwell_summary; Type: VIEW; Schema: public; Owner: -
--

CREATE VIEW public.trajectory_zone_dwell_summary AS
 SELECT zone_id,
    count(*) AS visit_count,
    avg(dwell_ms) AS avg_dwell_ms,
    sum(dwell_ms) AS total_dwell_ms,
    avg(confidence) AS avg_confidence
   FROM public.trajectory_segments
  WHERE (is_reliable = true)
  GROUP BY zone_id;


--
-- Name: trajectory_zone_transition_summary; Type: VIEW; Schema: public; Owner: -
--

CREATE VIEW public.trajectory_zone_transition_summary AS
 WITH ordered_segments AS (
         SELECT trajectory_segments.camera_id,
            trajectory_segments.global_id,
            trajectory_segments.zone_id,
            trajectory_segments.start_ms,
            lead(trajectory_segments.zone_id) OVER (PARTITION BY trajectory_segments.camera_id, trajectory_segments.global_id ORDER BY trajectory_segments.start_ms) AS next_zone_id
           FROM public.trajectory_segments
          WHERE ((trajectory_segments.is_reliable = true) AND (trajectory_segments.zone_id IS NOT NULL))
        )
 SELECT zone_id AS from_zone_id,
    next_zone_id AS to_zone_id,
    count(*) AS transition_count
   FROM ordered_segments
  WHERE ((next_zone_id IS NOT NULL) AND (zone_id <> next_zone_id))
  GROUP BY zone_id, next_zone_id;


--
-- Name: vms_session; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.vms_session (
    token_hash bytea NOT NULL,
    user_id integer NOT NULL,
    issued_at timestamp with time zone DEFAULT now() NOT NULL,
    expires_at timestamp with time zone NOT NULL,
    device text,
    CONSTRAINT vms_session_token_hash_check CHECK ((length(token_hash) = 32))
);


--
-- Name: vms_user; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.vms_user (
    user_id integer NOT NULL,
    username text NOT NULL,
    display_name text NOT NULL,
    pw_algo text DEFAULT 'pbkdf2-sha256'::text NOT NULL,
    pw_iters integer DEFAULT 200000 NOT NULL,
    pw_salt bytea NOT NULL,
    pw_hash bytea NOT NULL,
    role text NOT NULL,
    enabled boolean DEFAULT true NOT NULL,
    failed_count integer DEFAULT 0 NOT NULL,
    locked_until timestamp with time zone,
    last_login_at timestamp with time zone,
    created_at timestamp with time zone DEFAULT now() NOT NULL,
    must_change_pw boolean DEFAULT false NOT NULL,
    CONSTRAINT vms_user_pw_hash_check CHECK ((length(pw_hash) = 32)),
    CONSTRAINT vms_user_pw_iters_check CHECK ((pw_iters >= 1000)),
    CONSTRAINT vms_user_pw_salt_check CHECK ((length(pw_salt) >= 16)),
    CONSTRAINT vms_user_role_check CHECK ((role = ANY (ARRAY['admin'::text, 'operator'::text])))
);


--
-- Name: TABLE vms_user; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON TABLE public.vms_user IS 'VMS 로그인 계정. 비밀번호는 PBKDF2-HMAC-SHA256 해시로만 보관(원문 없음).';


--
-- Name: COLUMN vms_user.pw_iters; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.vms_user.pw_iters IS '해시 생성 당시의 반복 횟수. 행마다 달라도 되므로 나중에 상향 가능.';


--
-- Name: COLUMN vms_user.must_change_pw; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.vms_user.must_change_pw IS 'TRUE 면 로그인은 되지만 비밀번호를 바꾸기 전에는 쓰기 명령이 거부된다. 시드 계정(해시가 저장소에 공개)과 관리자가 만든 새 계정이 대상.';


--
-- Name: vms_user_user_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.vms_user_user_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: vms_user_user_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.vms_user_user_id_seq OWNED BY public.vms_user.user_id;


--
-- Name: zone_geometry_history; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.zone_geometry_history (
    geom_id bigint NOT NULL,
    zone_id integer NOT NULL,
    roi_polygon public.geometry(Polygon) NOT NULL,
    config_version integer NOT NULL,
    valid_from timestamp with time zone DEFAULT now() NOT NULL,
    valid_to timestamp with time zone,
    created_at timestamp with time zone DEFAULT now() NOT NULL
);


--
-- Name: zone_geometry_history_geom_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.zone_geometry_history_geom_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: zone_geometry_history_geom_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.zone_geometry_history_geom_id_seq OWNED BY public.zone_geometry_history.geom_id;


--
-- Name: zone_occupancy; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.zone_occupancy (
    zone_id integer NOT NULL,
    bucket_ts timestamp with time zone NOT NULL,
    person_count integer NOT NULL
);


--
-- Name: zone_thresholds; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.zone_thresholds (
    zone_id integer NOT NULL,
    capacity_limit integer,
    warn_ratio real DEFAULT 0.75 NOT NULL,
    critical_ratio real DEFAULT 0.90 NOT NULL,
    updated_at timestamp with time zone DEFAULT now() NOT NULL,
    CONSTRAINT chk_ratio_order CHECK ((warn_ratio < critical_ratio)),
    CONSTRAINT chk_ratio_range CHECK (((warn_ratio > (0)::double precision) AND (critical_ratio <= (1)::double precision)))
);


--
-- Name: zones; Type: TABLE; Schema: public; Owner: -
--

CREATE TABLE public.zones (
    zone_id integer NOT NULL,
    zone_name text NOT NULL,
    camera_id integer NOT NULL,
    channel integer NOT NULL,
    roi_polygon public.geometry(Polygon) NOT NULL,
    zone_type text DEFAULT 'normal'::text NOT NULL,
    config_version integer DEFAULT 1 NOT NULL
);


--
-- Name: COLUMN zones.config_version; Type: COMMENT; Schema: public; Owner: -
--

COMMENT ON COLUMN public.zones.config_version IS '현재 유효한 roi_polygon 의 형상 epoch. detections 태그와 대조되는 기준.';


--
-- Name: zones_zone_id_seq; Type: SEQUENCE; Schema: public; Owner: -
--

CREATE SEQUENCE public.zones_zone_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


--
-- Name: zones_zone_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: -
--

ALTER SEQUENCE public.zones_zone_id_seq OWNED BY public.zones.zone_id;


--
-- Name: detections_p20260730; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260730 FOR VALUES FROM ('2026-07-30 00:00:00+09') TO ('2026-07-31 00:00:00+09');


--
-- Name: detections_p20260731; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260731 FOR VALUES FROM ('2026-07-31 00:00:00+09') TO ('2026-08-01 00:00:00+09');


--
-- Name: detections_p20260801; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260801 FOR VALUES FROM ('2026-08-01 00:00:00+09') TO ('2026-08-02 00:00:00+09');


--
-- Name: detections_p20260802; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260802 FOR VALUES FROM ('2026-08-02 00:00:00+09') TO ('2026-08-03 00:00:00+09');


--
-- Name: detections_p20260803; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260803 FOR VALUES FROM ('2026-08-03 00:00:00+09') TO ('2026-08-04 00:00:00+09');


--
-- Name: detections_p20260804; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260804 FOR VALUES FROM ('2026-08-04 00:00:00+09') TO ('2026-08-05 00:00:00+09');


--
-- Name: detections_p20260805; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260805 FOR VALUES FROM ('2026-08-05 00:00:00+09') TO ('2026-08-06 00:00:00+09');


--
-- Name: detections_p20260806; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260806 FOR VALUES FROM ('2026-08-06 00:00:00+09') TO ('2026-08-07 00:00:00+09');


--
-- Name: detections_p20260807; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260807 FOR VALUES FROM ('2026-08-07 00:00:00+09') TO ('2026-08-08 00:00:00+09');


--
-- Name: detections_p20260808; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260808 FOR VALUES FROM ('2026-08-08 00:00:00+09') TO ('2026-08-09 00:00:00+09');


--
-- Name: detections_p20260809; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260809 FOR VALUES FROM ('2026-08-09 00:00:00+09') TO ('2026-08-10 00:00:00+09');


--
-- Name: detections_p20260810; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260810 FOR VALUES FROM ('2026-08-10 00:00:00+09') TO ('2026-08-11 00:00:00+09');


--
-- Name: detections_p20260811; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260811 FOR VALUES FROM ('2026-08-11 00:00:00+09') TO ('2026-08-12 00:00:00+09');


--
-- Name: detections_p20260812; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260812 FOR VALUES FROM ('2026-08-12 00:00:00+09') TO ('2026-08-13 00:00:00+09');


--
-- Name: detections_p20260813; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260813 FOR VALUES FROM ('2026-08-13 00:00:00+09') TO ('2026-08-14 00:00:00+09');


--
-- Name: detections_p20260814; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260814 FOR VALUES FROM ('2026-08-14 00:00:00+09') TO ('2026-08-15 00:00:00+09');


--
-- Name: detections_p20260815; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260815 FOR VALUES FROM ('2026-08-15 00:00:00+09') TO ('2026-08-16 00:00:00+09');


--
-- Name: detections_p20260816; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260816 FOR VALUES FROM ('2026-08-16 00:00:00+09') TO ('2026-08-17 00:00:00+09');


--
-- Name: detections_p20260817; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260817 FOR VALUES FROM ('2026-08-17 00:00:00+09') TO ('2026-08-18 00:00:00+09');


--
-- Name: detections_p20260818; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260818 FOR VALUES FROM ('2026-08-18 00:00:00+09') TO ('2026-08-19 00:00:00+09');


--
-- Name: detections_p20260819; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260819 FOR VALUES FROM ('2026-08-19 00:00:00+09') TO ('2026-08-20 00:00:00+09');


--
-- Name: detections_p20260820; Type: TABLE ATTACH; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ATTACH PARTITION public.detections_p20260820 FOR VALUES FROM ('2026-08-20 00:00:00+09') TO ('2026-08-21 00:00:00+09');


--
-- Name: alerts alert_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.alerts ALTER COLUMN alert_id SET DEFAULT nextval('public.alerts_alert_id_seq'::regclass);


--
-- Name: button_event button_event_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.button_event ALTER COLUMN button_event_id SET DEFAULT nextval('public.button_event_button_event_id_seq'::regclass);


--
-- Name: cameras camera_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.cameras ALTER COLUMN camera_id SET DEFAULT nextval('public.cameras_camera_id_seq'::regclass);


--
-- Name: congestion_prediction prediction_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.congestion_prediction ALTER COLUMN prediction_id SET DEFAULT nextval('public.congestion_prediction_prediction_id_seq'::regclass);


--
-- Name: detections detection_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections ALTER COLUMN detection_id SET DEFAULT nextval('public.detections_detection_id_seq'::regclass);


--
-- Name: faces face_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.faces ALTER COLUMN face_id SET DEFAULT nextval('public.faces_face_id_seq'::regclass);


--
-- Name: fire_event event_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_event ALTER COLUMN event_id SET DEFAULT nextval('public.fire_event_event_id_seq'::regclass);


--
-- Name: fire_threshold threshold_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_threshold ALTER COLUMN threshold_id SET DEFAULT nextval('public.fire_threshold_threshold_id_seq'::regclass);


--
-- Name: incidents incident_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.incidents ALTER COLUMN incident_id SET DEFAULT nextval('public.incidents_incident_id_seq'::regclass);


--
-- Name: manual_command manual_command_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.manual_command ALTER COLUMN manual_command_id SET DEFAULT nextval('public.manual_command_manual_command_id_seq'::regclass);


--
-- Name: sensor_reading reading_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.sensor_reading ALTER COLUMN reading_id SET DEFAULT nextval('public.sensor_reading_reading_id_seq'::regclass);


--
-- Name: tracks track_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.tracks ALTER COLUMN track_id SET DEFAULT nextval('public.tracks_track_id_seq'::regclass);


--
-- Name: vms_user user_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.vms_user ALTER COLUMN user_id SET DEFAULT nextval('public.vms_user_user_id_seq'::regclass);


--
-- Name: zone_geometry_history geom_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.zone_geometry_history ALTER COLUMN geom_id SET DEFAULT nextval('public.zone_geometry_history_geom_id_seq'::regclass);


--
-- Name: zones zone_id; Type: DEFAULT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.zones ALTER COLUMN zone_id SET DEFAULT nextval('public.zones_zone_id_seq'::regclass);


--
-- Name: actuator_command actuator_command_command_key_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.actuator_command
    ADD CONSTRAINT actuator_command_command_key_key UNIQUE (command_key);


--
-- Name: actuator_command actuator_command_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.actuator_command
    ADD CONSTRAINT actuator_command_pkey PRIMARY KEY (command_id);


--
-- Name: alerts alerts_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.alerts
    ADD CONSTRAINT alerts_pkey PRIMARY KEY (alert_id);


--
-- Name: button_event button_event_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.button_event
    ADD CONSTRAINT button_event_pkey PRIMARY KEY (button_event_id);


--
-- Name: camera_credentials camera_credentials_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.camera_credentials
    ADD CONSTRAINT camera_credentials_pkey PRIMARY KEY (camera_id);


--
-- Name: cameras cameras_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.cameras
    ADD CONSTRAINT cameras_pkey PRIMARY KEY (camera_id);


--
-- Name: congestion_prediction congestion_prediction_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.congestion_prediction
    ADD CONSTRAINT congestion_prediction_pkey PRIMARY KEY (prediction_id);


--
-- Name: detections detections_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections
    ADD CONSTRAINT detections_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260730 detections_p20260730_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260730
    ADD CONSTRAINT detections_p20260730_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260731 detections_p20260731_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260731
    ADD CONSTRAINT detections_p20260731_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260801 detections_p20260801_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260801
    ADD CONSTRAINT detections_p20260801_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260802 detections_p20260802_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260802
    ADD CONSTRAINT detections_p20260802_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260803 detections_p20260803_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260803
    ADD CONSTRAINT detections_p20260803_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260804 detections_p20260804_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260804
    ADD CONSTRAINT detections_p20260804_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260805 detections_p20260805_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260805
    ADD CONSTRAINT detections_p20260805_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260806 detections_p20260806_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260806
    ADD CONSTRAINT detections_p20260806_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260807 detections_p20260807_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260807
    ADD CONSTRAINT detections_p20260807_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260808 detections_p20260808_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260808
    ADD CONSTRAINT detections_p20260808_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260809 detections_p20260809_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260809
    ADD CONSTRAINT detections_p20260809_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260810 detections_p20260810_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260810
    ADD CONSTRAINT detections_p20260810_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260811 detections_p20260811_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260811
    ADD CONSTRAINT detections_p20260811_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260812 detections_p20260812_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260812
    ADD CONSTRAINT detections_p20260812_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260813 detections_p20260813_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260813
    ADD CONSTRAINT detections_p20260813_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260814 detections_p20260814_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260814
    ADD CONSTRAINT detections_p20260814_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260815 detections_p20260815_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260815
    ADD CONSTRAINT detections_p20260815_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260816 detections_p20260816_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260816
    ADD CONSTRAINT detections_p20260816_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260817 detections_p20260817_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260817
    ADD CONSTRAINT detections_p20260817_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260818 detections_p20260818_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260818
    ADD CONSTRAINT detections_p20260818_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260819 detections_p20260819_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260819
    ADD CONSTRAINT detections_p20260819_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: detections_p20260820 detections_p20260820_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.detections_p20260820
    ADD CONSTRAINT detections_p20260820_pkey PRIMARY KEY (detection_id, ts);


--
-- Name: endpoints endpoints_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.endpoints
    ADD CONSTRAINT endpoints_pkey PRIMARY KEY (key);


--
-- Name: faces faces_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.faces
    ADD CONSTRAINT faces_pkey PRIMARY KEY (face_id);


--
-- Name: fire_event_command fire_event_command_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_event_command
    ADD CONSTRAINT fire_event_command_pkey PRIMARY KEY (event_id, command_id);


--
-- Name: fire_event fire_event_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_event
    ADD CONSTRAINT fire_event_pkey PRIMARY KEY (event_id);


--
-- Name: fire_threshold fire_threshold_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_threshold
    ADD CONSTRAINT fire_threshold_pkey PRIMARY KEY (threshold_id);


--
-- Name: fire_zone fire_zone_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_zone
    ADD CONSTRAINT fire_zone_pkey PRIMARY KEY (zone_id);


--
-- Name: fire_zone fire_zone_rpia_node_id_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_zone
    ADD CONSTRAINT fire_zone_rpia_node_id_key UNIQUE (rpia_node_id);


--
-- Name: fire_zone fire_zone_rpic_node_id_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_zone
    ADD CONSTRAINT fire_zone_rpic_node_id_key UNIQUE (rpic_node_id);


--
-- Name: incidents incidents_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.incidents
    ADD CONSTRAINT incidents_pkey PRIMARY KEY (incident_id);


--
-- Name: line_flow line_flow_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.line_flow
    ADD CONSTRAINT line_flow_pkey PRIMARY KEY (rule, action, bucket_ts);


--
-- Name: manual_command manual_command_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.manual_command
    ADD CONSTRAINT manual_command_pkey PRIMARY KEY (manual_command_id);


--
-- Name: schema_migrations schema_migrations_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.schema_migrations
    ADD CONSTRAINT schema_migrations_pkey PRIMARY KEY (version);


--
-- Name: season_threshold season_threshold_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.season_threshold
    ADD CONSTRAINT season_threshold_pkey PRIMARY KEY (season_key);


--
-- Name: sensor_channel sensor_channel_channel_key_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.sensor_channel
    ADD CONSTRAINT sensor_channel_channel_key_key UNIQUE (channel_key);


--
-- Name: sensor_channel sensor_channel_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.sensor_channel
    ADD CONSTRAINT sensor_channel_pkey PRIMARY KEY (channel_id);


--
-- Name: sensor_reading sensor_reading_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.sensor_reading
    ADD CONSTRAINT sensor_reading_pkey PRIMARY KEY (reading_id);


--
-- Name: sensor_value sensor_value_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.sensor_value
    ADD CONSTRAINT sensor_value_pkey PRIMARY KEY (reading_id, channel_id);


--
-- Name: site_config site_config_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.site_config
    ADD CONSTRAINT site_config_pkey PRIMARY KEY (key);


--
-- Name: track_path track_path_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.track_path
    ADD CONSTRAINT track_path_pkey PRIMARY KEY (track_id, ts);


--
-- Name: tracks tracks_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.tracks
    ADD CONSTRAINT tracks_pkey PRIMARY KEY (track_id);


--
-- Name: trajectory_segments trajectory_segments_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.trajectory_segments
    ADD CONSTRAINT trajectory_segments_pkey PRIMARY KEY (camera_id, segment_id, start_ms);


--
-- Name: vms_session vms_session_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.vms_session
    ADD CONSTRAINT vms_session_pkey PRIMARY KEY (token_hash);


--
-- Name: vms_user vms_user_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.vms_user
    ADD CONSTRAINT vms_user_pkey PRIMARY KEY (user_id);


--
-- Name: vms_user vms_user_username_key; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.vms_user
    ADD CONSTRAINT vms_user_username_key UNIQUE (username);


--
-- Name: zone_geometry_history zone_geometry_history_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.zone_geometry_history
    ADD CONSTRAINT zone_geometry_history_pkey PRIMARY KEY (geom_id);


--
-- Name: zone_occupancy zone_occupancy_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.zone_occupancy
    ADD CONSTRAINT zone_occupancy_pkey PRIMARY KEY (zone_id, bucket_ts);


--
-- Name: zone_thresholds zone_thresholds_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.zone_thresholds
    ADD CONSTRAINT zone_thresholds_pkey PRIMARY KEY (zone_id);


--
-- Name: zones zones_pkey; Type: CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.zones
    ADD CONSTRAINT zones_pkey PRIMARY KEY (zone_id);


--
-- Name: idx_detections_camera; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_detections_camera ON ONLY public.detections USING btree (camera_id);


--
-- Name: detections_p20260730_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260730_camera_id_idx ON public.detections_p20260730 USING btree (camera_id);


--
-- Name: idx_detections_display_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_detections_display_ts ON ONLY public.detections USING btree (display_id, ts);


--
-- Name: detections_p20260730_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260730_display_id_ts_idx ON public.detections_p20260730 USING btree (display_id, ts);


--
-- Name: idx_detections_geom; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_detections_geom ON ONLY public.detections USING gist (geom);


--
-- Name: detections_p20260730_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260730_geom_idx ON public.detections_p20260730 USING gist (geom);


--
-- Name: idx_detections_objid; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_detections_objid ON ONLY public.detections USING btree (object_id, ts);


--
-- Name: detections_p20260730_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260730_object_id_ts_idx ON public.detections_p20260730 USING btree (object_id, ts);


--
-- Name: idx_detections_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_detections_ts ON ONLY public.detections USING btree (ts);


--
-- Name: detections_p20260730_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260730_ts_idx ON public.detections_p20260730 USING btree (ts);


--
-- Name: detections_p20260731_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260731_camera_id_idx ON public.detections_p20260731 USING btree (camera_id);


--
-- Name: detections_p20260731_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260731_display_id_ts_idx ON public.detections_p20260731 USING btree (display_id, ts);


--
-- Name: detections_p20260731_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260731_geom_idx ON public.detections_p20260731 USING gist (geom);


--
-- Name: detections_p20260731_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260731_object_id_ts_idx ON public.detections_p20260731 USING btree (object_id, ts);


--
-- Name: detections_p20260731_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260731_ts_idx ON public.detections_p20260731 USING btree (ts);


--
-- Name: detections_p20260801_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260801_camera_id_idx ON public.detections_p20260801 USING btree (camera_id);


--
-- Name: detections_p20260801_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260801_display_id_ts_idx ON public.detections_p20260801 USING btree (display_id, ts);


--
-- Name: detections_p20260801_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260801_geom_idx ON public.detections_p20260801 USING gist (geom);


--
-- Name: detections_p20260801_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260801_object_id_ts_idx ON public.detections_p20260801 USING btree (object_id, ts);


--
-- Name: detections_p20260801_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260801_ts_idx ON public.detections_p20260801 USING btree (ts);


--
-- Name: detections_p20260802_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260802_camera_id_idx ON public.detections_p20260802 USING btree (camera_id);


--
-- Name: detections_p20260802_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260802_display_id_ts_idx ON public.detections_p20260802 USING btree (display_id, ts);


--
-- Name: detections_p20260802_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260802_geom_idx ON public.detections_p20260802 USING gist (geom);


--
-- Name: detections_p20260802_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260802_object_id_ts_idx ON public.detections_p20260802 USING btree (object_id, ts);


--
-- Name: detections_p20260802_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260802_ts_idx ON public.detections_p20260802 USING btree (ts);


--
-- Name: detections_p20260803_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260803_camera_id_idx ON public.detections_p20260803 USING btree (camera_id);


--
-- Name: detections_p20260803_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260803_display_id_ts_idx ON public.detections_p20260803 USING btree (display_id, ts);


--
-- Name: detections_p20260803_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260803_geom_idx ON public.detections_p20260803 USING gist (geom);


--
-- Name: detections_p20260803_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260803_object_id_ts_idx ON public.detections_p20260803 USING btree (object_id, ts);


--
-- Name: detections_p20260803_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260803_ts_idx ON public.detections_p20260803 USING btree (ts);


--
-- Name: detections_p20260804_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260804_camera_id_idx ON public.detections_p20260804 USING btree (camera_id);


--
-- Name: detections_p20260804_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260804_display_id_ts_idx ON public.detections_p20260804 USING btree (display_id, ts);


--
-- Name: detections_p20260804_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260804_geom_idx ON public.detections_p20260804 USING gist (geom);


--
-- Name: detections_p20260804_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260804_object_id_ts_idx ON public.detections_p20260804 USING btree (object_id, ts);


--
-- Name: detections_p20260804_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260804_ts_idx ON public.detections_p20260804 USING btree (ts);


--
-- Name: detections_p20260805_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260805_camera_id_idx ON public.detections_p20260805 USING btree (camera_id);


--
-- Name: detections_p20260805_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260805_display_id_ts_idx ON public.detections_p20260805 USING btree (display_id, ts);


--
-- Name: detections_p20260805_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260805_geom_idx ON public.detections_p20260805 USING gist (geom);


--
-- Name: detections_p20260805_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260805_object_id_ts_idx ON public.detections_p20260805 USING btree (object_id, ts);


--
-- Name: detections_p20260805_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260805_ts_idx ON public.detections_p20260805 USING btree (ts);


--
-- Name: detections_p20260806_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260806_camera_id_idx ON public.detections_p20260806 USING btree (camera_id);


--
-- Name: detections_p20260806_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260806_display_id_ts_idx ON public.detections_p20260806 USING btree (display_id, ts);


--
-- Name: detections_p20260806_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260806_geom_idx ON public.detections_p20260806 USING gist (geom);


--
-- Name: detections_p20260806_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260806_object_id_ts_idx ON public.detections_p20260806 USING btree (object_id, ts);


--
-- Name: detections_p20260806_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260806_ts_idx ON public.detections_p20260806 USING btree (ts);


--
-- Name: detections_p20260807_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260807_camera_id_idx ON public.detections_p20260807 USING btree (camera_id);


--
-- Name: detections_p20260807_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260807_display_id_ts_idx ON public.detections_p20260807 USING btree (display_id, ts);


--
-- Name: detections_p20260807_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260807_geom_idx ON public.detections_p20260807 USING gist (geom);


--
-- Name: detections_p20260807_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260807_object_id_ts_idx ON public.detections_p20260807 USING btree (object_id, ts);


--
-- Name: detections_p20260807_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260807_ts_idx ON public.detections_p20260807 USING btree (ts);


--
-- Name: detections_p20260808_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260808_camera_id_idx ON public.detections_p20260808 USING btree (camera_id);


--
-- Name: detections_p20260808_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260808_display_id_ts_idx ON public.detections_p20260808 USING btree (display_id, ts);


--
-- Name: detections_p20260808_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260808_geom_idx ON public.detections_p20260808 USING gist (geom);


--
-- Name: detections_p20260808_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260808_object_id_ts_idx ON public.detections_p20260808 USING btree (object_id, ts);


--
-- Name: detections_p20260808_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260808_ts_idx ON public.detections_p20260808 USING btree (ts);


--
-- Name: detections_p20260809_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260809_camera_id_idx ON public.detections_p20260809 USING btree (camera_id);


--
-- Name: detections_p20260809_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260809_display_id_ts_idx ON public.detections_p20260809 USING btree (display_id, ts);


--
-- Name: detections_p20260809_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260809_geom_idx ON public.detections_p20260809 USING gist (geom);


--
-- Name: detections_p20260809_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260809_object_id_ts_idx ON public.detections_p20260809 USING btree (object_id, ts);


--
-- Name: detections_p20260809_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260809_ts_idx ON public.detections_p20260809 USING btree (ts);


--
-- Name: detections_p20260810_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260810_camera_id_idx ON public.detections_p20260810 USING btree (camera_id);


--
-- Name: detections_p20260810_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260810_display_id_ts_idx ON public.detections_p20260810 USING btree (display_id, ts);


--
-- Name: detections_p20260810_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260810_geom_idx ON public.detections_p20260810 USING gist (geom);


--
-- Name: detections_p20260810_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260810_object_id_ts_idx ON public.detections_p20260810 USING btree (object_id, ts);


--
-- Name: detections_p20260810_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260810_ts_idx ON public.detections_p20260810 USING btree (ts);


--
-- Name: detections_p20260811_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260811_camera_id_idx ON public.detections_p20260811 USING btree (camera_id);


--
-- Name: detections_p20260811_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260811_display_id_ts_idx ON public.detections_p20260811 USING btree (display_id, ts);


--
-- Name: detections_p20260811_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260811_geom_idx ON public.detections_p20260811 USING gist (geom);


--
-- Name: detections_p20260811_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260811_object_id_ts_idx ON public.detections_p20260811 USING btree (object_id, ts);


--
-- Name: detections_p20260811_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260811_ts_idx ON public.detections_p20260811 USING btree (ts);


--
-- Name: detections_p20260812_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260812_camera_id_idx ON public.detections_p20260812 USING btree (camera_id);


--
-- Name: detections_p20260812_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260812_display_id_ts_idx ON public.detections_p20260812 USING btree (display_id, ts);


--
-- Name: detections_p20260812_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260812_geom_idx ON public.detections_p20260812 USING gist (geom);


--
-- Name: detections_p20260812_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260812_object_id_ts_idx ON public.detections_p20260812 USING btree (object_id, ts);


--
-- Name: detections_p20260812_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260812_ts_idx ON public.detections_p20260812 USING btree (ts);


--
-- Name: detections_p20260813_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260813_camera_id_idx ON public.detections_p20260813 USING btree (camera_id);


--
-- Name: detections_p20260813_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260813_display_id_ts_idx ON public.detections_p20260813 USING btree (display_id, ts);


--
-- Name: detections_p20260813_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260813_geom_idx ON public.detections_p20260813 USING gist (geom);


--
-- Name: detections_p20260813_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260813_object_id_ts_idx ON public.detections_p20260813 USING btree (object_id, ts);


--
-- Name: detections_p20260813_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260813_ts_idx ON public.detections_p20260813 USING btree (ts);


--
-- Name: detections_p20260814_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260814_camera_id_idx ON public.detections_p20260814 USING btree (camera_id);


--
-- Name: detections_p20260814_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260814_display_id_ts_idx ON public.detections_p20260814 USING btree (display_id, ts);


--
-- Name: detections_p20260814_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260814_geom_idx ON public.detections_p20260814 USING gist (geom);


--
-- Name: detections_p20260814_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260814_object_id_ts_idx ON public.detections_p20260814 USING btree (object_id, ts);


--
-- Name: detections_p20260814_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260814_ts_idx ON public.detections_p20260814 USING btree (ts);


--
-- Name: detections_p20260815_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260815_camera_id_idx ON public.detections_p20260815 USING btree (camera_id);


--
-- Name: detections_p20260815_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260815_display_id_ts_idx ON public.detections_p20260815 USING btree (display_id, ts);


--
-- Name: detections_p20260815_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260815_geom_idx ON public.detections_p20260815 USING gist (geom);


--
-- Name: detections_p20260815_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260815_object_id_ts_idx ON public.detections_p20260815 USING btree (object_id, ts);


--
-- Name: detections_p20260815_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260815_ts_idx ON public.detections_p20260815 USING btree (ts);


--
-- Name: detections_p20260816_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260816_camera_id_idx ON public.detections_p20260816 USING btree (camera_id);


--
-- Name: detections_p20260816_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260816_display_id_ts_idx ON public.detections_p20260816 USING btree (display_id, ts);


--
-- Name: detections_p20260816_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260816_geom_idx ON public.detections_p20260816 USING gist (geom);


--
-- Name: detections_p20260816_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260816_object_id_ts_idx ON public.detections_p20260816 USING btree (object_id, ts);


--
-- Name: detections_p20260816_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260816_ts_idx ON public.detections_p20260816 USING btree (ts);


--
-- Name: detections_p20260817_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260817_camera_id_idx ON public.detections_p20260817 USING btree (camera_id);


--
-- Name: detections_p20260817_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260817_display_id_ts_idx ON public.detections_p20260817 USING btree (display_id, ts);


--
-- Name: detections_p20260817_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260817_geom_idx ON public.detections_p20260817 USING gist (geom);


--
-- Name: detections_p20260817_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260817_object_id_ts_idx ON public.detections_p20260817 USING btree (object_id, ts);


--
-- Name: detections_p20260817_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260817_ts_idx ON public.detections_p20260817 USING btree (ts);


--
-- Name: detections_p20260818_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260818_camera_id_idx ON public.detections_p20260818 USING btree (camera_id);


--
-- Name: detections_p20260818_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260818_display_id_ts_idx ON public.detections_p20260818 USING btree (display_id, ts);


--
-- Name: detections_p20260818_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260818_geom_idx ON public.detections_p20260818 USING gist (geom);


--
-- Name: detections_p20260818_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260818_object_id_ts_idx ON public.detections_p20260818 USING btree (object_id, ts);


--
-- Name: detections_p20260818_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260818_ts_idx ON public.detections_p20260818 USING btree (ts);


--
-- Name: detections_p20260819_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260819_camera_id_idx ON public.detections_p20260819 USING btree (camera_id);


--
-- Name: detections_p20260819_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260819_display_id_ts_idx ON public.detections_p20260819 USING btree (display_id, ts);


--
-- Name: detections_p20260819_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260819_geom_idx ON public.detections_p20260819 USING gist (geom);


--
-- Name: detections_p20260819_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260819_object_id_ts_idx ON public.detections_p20260819 USING btree (object_id, ts);


--
-- Name: detections_p20260819_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260819_ts_idx ON public.detections_p20260819 USING btree (ts);


--
-- Name: detections_p20260820_camera_id_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260820_camera_id_idx ON public.detections_p20260820 USING btree (camera_id);


--
-- Name: detections_p20260820_display_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260820_display_id_ts_idx ON public.detections_p20260820 USING btree (display_id, ts);


--
-- Name: detections_p20260820_geom_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260820_geom_idx ON public.detections_p20260820 USING gist (geom);


--
-- Name: detections_p20260820_object_id_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260820_object_id_ts_idx ON public.detections_p20260820 USING btree (object_id, ts);


--
-- Name: detections_p20260820_ts_idx; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX detections_p20260820_ts_idx ON public.detections_p20260820 USING btree (ts);


--
-- Name: idx_button_event_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_button_event_ts ON public.button_event USING btree (occurred_at);


--
-- Name: idx_faces_objid; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_faces_objid ON public.faces USING btree (object_id, ts);


--
-- Name: idx_faces_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_faces_ts ON public.faces USING btree (ts);


--
-- Name: idx_fire_event_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_fire_event_ts ON public.fire_event USING btree (occurred_at);


--
-- Name: idx_fire_event_type; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_fire_event_type ON public.fire_event USING btree (event_type);


--
-- Name: idx_incidents_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_incidents_ts ON public.incidents USING btree (detected_at);


--
-- Name: idx_incidents_zone; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_incidents_zone ON public.incidents USING btree (zone_id);


--
-- Name: idx_line_flow_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_line_flow_ts ON public.line_flow USING btree (bucket_ts);


--
-- Name: idx_manual_command_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_manual_command_ts ON public.manual_command USING btree (issued_at);


--
-- Name: idx_occupancy_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_occupancy_ts ON public.zone_occupancy USING btree (bucket_ts);


--
-- Name: idx_prediction_target; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_prediction_target ON public.congestion_prediction USING btree (zone_id, target_ts);


--
-- Name: idx_sensor_reading_seq; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_sensor_reading_seq ON public.sensor_reading USING btree (sensor_seq);


--
-- Name: idx_sensor_reading_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_sensor_reading_ts ON public.sensor_reading USING btree (received_at);


--
-- Name: idx_sensor_reading_zone_latest; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_sensor_reading_zone_latest ON public.sensor_reading USING btree (zone_id, reading_id DESC);


--
-- Name: idx_sensor_value_channel; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_sensor_value_channel ON public.sensor_value USING btree (channel_id, reading_id);


--
-- Name: idx_track_path_display_ts; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_track_path_display_ts ON public.track_path USING btree (display_id, ts);


--
-- Name: idx_track_path_geom; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_track_path_geom ON public.track_path USING gist (geom);


--
-- Name: idx_track_path_zone; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_track_path_zone ON public.track_path USING btree (zone_id);


--
-- Name: idx_tracks_objid; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_tracks_objid ON public.tracks USING btree (object_id);


--
-- Name: idx_trajectory_segments_display; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_trajectory_segments_display ON public.trajectory_segments USING btree (display_id, segment_ts);


--
-- Name: idx_trajectory_segments_global_start; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_trajectory_segments_global_start ON public.trajectory_segments USING btree (camera_id, global_id, start_ms);


--
-- Name: idx_trajectory_segments_served; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_trajectory_segments_served ON public.trajectory_segments USING btree (served_at);


--
-- Name: idx_trajectory_segments_zone_reliable; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_trajectory_segments_zone_reliable ON public.trajectory_segments USING btree (zone_id, is_reliable, segment_ts);


--
-- Name: idx_vms_session_expires; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_vms_session_expires ON public.vms_session USING btree (expires_at);


--
-- Name: idx_vms_session_user; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_vms_session_user ON public.vms_session USING btree (user_id);


--
-- Name: idx_zgh_geom; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_zgh_geom ON public.zone_geometry_history USING gist (roi_polygon);


--
-- Name: idx_zgh_zone; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_zgh_zone ON public.zone_geometry_history USING btree (zone_id);


--
-- Name: idx_zgh_zone_valid; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_zgh_zone_valid ON public.zone_geometry_history USING btree (zone_id, valid_from, valid_to);


--
-- Name: idx_zones_roi; Type: INDEX; Schema: public; Owner: -
--

CREATE INDEX idx_zones_roi ON public.zones USING gist (roi_polygon);


--
-- Name: uq_fire_threshold_active; Type: INDEX; Schema: public; Owner: -
--

CREATE UNIQUE INDEX uq_fire_threshold_active ON public.fire_threshold USING btree (is_active) WHERE is_active;


--
-- Name: uq_tracks_display_id; Type: INDEX; Schema: public; Owner: -
--

CREATE UNIQUE INDEX uq_tracks_display_id ON public.tracks USING btree (display_id);


--
-- Name: uq_zgh_open; Type: INDEX; Schema: public; Owner: -
--

CREATE UNIQUE INDEX uq_zgh_open ON public.zone_geometry_history USING btree (zone_id) WHERE (valid_to IS NULL);


--
-- Name: uq_zones_camera_channel; Type: INDEX; Schema: public; Owner: -
--

CREATE UNIQUE INDEX uq_zones_camera_channel ON public.zones USING btree (camera_id, channel);


--
-- Name: detections_p20260730_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260730_camera_id_idx;


--
-- Name: detections_p20260730_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260730_display_id_ts_idx;


--
-- Name: detections_p20260730_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260730_geom_idx;


--
-- Name: detections_p20260730_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260730_object_id_ts_idx;


--
-- Name: detections_p20260730_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260730_pkey;


--
-- Name: detections_p20260730_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260730_ts_idx;


--
-- Name: detections_p20260731_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260731_camera_id_idx;


--
-- Name: detections_p20260731_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260731_display_id_ts_idx;


--
-- Name: detections_p20260731_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260731_geom_idx;


--
-- Name: detections_p20260731_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260731_object_id_ts_idx;


--
-- Name: detections_p20260731_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260731_pkey;


--
-- Name: detections_p20260731_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260731_ts_idx;


--
-- Name: detections_p20260801_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260801_camera_id_idx;


--
-- Name: detections_p20260801_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260801_display_id_ts_idx;


--
-- Name: detections_p20260801_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260801_geom_idx;


--
-- Name: detections_p20260801_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260801_object_id_ts_idx;


--
-- Name: detections_p20260801_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260801_pkey;


--
-- Name: detections_p20260801_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260801_ts_idx;


--
-- Name: detections_p20260802_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260802_camera_id_idx;


--
-- Name: detections_p20260802_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260802_display_id_ts_idx;


--
-- Name: detections_p20260802_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260802_geom_idx;


--
-- Name: detections_p20260802_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260802_object_id_ts_idx;


--
-- Name: detections_p20260802_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260802_pkey;


--
-- Name: detections_p20260802_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260802_ts_idx;


--
-- Name: detections_p20260803_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260803_camera_id_idx;


--
-- Name: detections_p20260803_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260803_display_id_ts_idx;


--
-- Name: detections_p20260803_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260803_geom_idx;


--
-- Name: detections_p20260803_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260803_object_id_ts_idx;


--
-- Name: detections_p20260803_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260803_pkey;


--
-- Name: detections_p20260803_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260803_ts_idx;


--
-- Name: detections_p20260804_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260804_camera_id_idx;


--
-- Name: detections_p20260804_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260804_display_id_ts_idx;


--
-- Name: detections_p20260804_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260804_geom_idx;


--
-- Name: detections_p20260804_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260804_object_id_ts_idx;


--
-- Name: detections_p20260804_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260804_pkey;


--
-- Name: detections_p20260804_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260804_ts_idx;


--
-- Name: detections_p20260805_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260805_camera_id_idx;


--
-- Name: detections_p20260805_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260805_display_id_ts_idx;


--
-- Name: detections_p20260805_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260805_geom_idx;


--
-- Name: detections_p20260805_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260805_object_id_ts_idx;


--
-- Name: detections_p20260805_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260805_pkey;


--
-- Name: detections_p20260805_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260805_ts_idx;


--
-- Name: detections_p20260806_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260806_camera_id_idx;


--
-- Name: detections_p20260806_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260806_display_id_ts_idx;


--
-- Name: detections_p20260806_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260806_geom_idx;


--
-- Name: detections_p20260806_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260806_object_id_ts_idx;


--
-- Name: detections_p20260806_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260806_pkey;


--
-- Name: detections_p20260806_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260806_ts_idx;


--
-- Name: detections_p20260807_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260807_camera_id_idx;


--
-- Name: detections_p20260807_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260807_display_id_ts_idx;


--
-- Name: detections_p20260807_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260807_geom_idx;


--
-- Name: detections_p20260807_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260807_object_id_ts_idx;


--
-- Name: detections_p20260807_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260807_pkey;


--
-- Name: detections_p20260807_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260807_ts_idx;


--
-- Name: detections_p20260808_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260808_camera_id_idx;


--
-- Name: detections_p20260808_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260808_display_id_ts_idx;


--
-- Name: detections_p20260808_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260808_geom_idx;


--
-- Name: detections_p20260808_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260808_object_id_ts_idx;


--
-- Name: detections_p20260808_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260808_pkey;


--
-- Name: detections_p20260808_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260808_ts_idx;


--
-- Name: detections_p20260809_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260809_camera_id_idx;


--
-- Name: detections_p20260809_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260809_display_id_ts_idx;


--
-- Name: detections_p20260809_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260809_geom_idx;


--
-- Name: detections_p20260809_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260809_object_id_ts_idx;


--
-- Name: detections_p20260809_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260809_pkey;


--
-- Name: detections_p20260809_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260809_ts_idx;


--
-- Name: detections_p20260810_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260810_camera_id_idx;


--
-- Name: detections_p20260810_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260810_display_id_ts_idx;


--
-- Name: detections_p20260810_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260810_geom_idx;


--
-- Name: detections_p20260810_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260810_object_id_ts_idx;


--
-- Name: detections_p20260810_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260810_pkey;


--
-- Name: detections_p20260810_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260810_ts_idx;


--
-- Name: detections_p20260811_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260811_camera_id_idx;


--
-- Name: detections_p20260811_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260811_display_id_ts_idx;


--
-- Name: detections_p20260811_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260811_geom_idx;


--
-- Name: detections_p20260811_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260811_object_id_ts_idx;


--
-- Name: detections_p20260811_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260811_pkey;


--
-- Name: detections_p20260811_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260811_ts_idx;


--
-- Name: detections_p20260812_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260812_camera_id_idx;


--
-- Name: detections_p20260812_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260812_display_id_ts_idx;


--
-- Name: detections_p20260812_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260812_geom_idx;


--
-- Name: detections_p20260812_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260812_object_id_ts_idx;


--
-- Name: detections_p20260812_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260812_pkey;


--
-- Name: detections_p20260812_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260812_ts_idx;


--
-- Name: detections_p20260813_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260813_camera_id_idx;


--
-- Name: detections_p20260813_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260813_display_id_ts_idx;


--
-- Name: detections_p20260813_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260813_geom_idx;


--
-- Name: detections_p20260813_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260813_object_id_ts_idx;


--
-- Name: detections_p20260813_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260813_pkey;


--
-- Name: detections_p20260813_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260813_ts_idx;


--
-- Name: detections_p20260814_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260814_camera_id_idx;


--
-- Name: detections_p20260814_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260814_display_id_ts_idx;


--
-- Name: detections_p20260814_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260814_geom_idx;


--
-- Name: detections_p20260814_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260814_object_id_ts_idx;


--
-- Name: detections_p20260814_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260814_pkey;


--
-- Name: detections_p20260814_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260814_ts_idx;


--
-- Name: detections_p20260815_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260815_camera_id_idx;


--
-- Name: detections_p20260815_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260815_display_id_ts_idx;


--
-- Name: detections_p20260815_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260815_geom_idx;


--
-- Name: detections_p20260815_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260815_object_id_ts_idx;


--
-- Name: detections_p20260815_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260815_pkey;


--
-- Name: detections_p20260815_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260815_ts_idx;


--
-- Name: detections_p20260816_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260816_camera_id_idx;


--
-- Name: detections_p20260816_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260816_display_id_ts_idx;


--
-- Name: detections_p20260816_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260816_geom_idx;


--
-- Name: detections_p20260816_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260816_object_id_ts_idx;


--
-- Name: detections_p20260816_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260816_pkey;


--
-- Name: detections_p20260816_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260816_ts_idx;


--
-- Name: detections_p20260817_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260817_camera_id_idx;


--
-- Name: detections_p20260817_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260817_display_id_ts_idx;


--
-- Name: detections_p20260817_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260817_geom_idx;


--
-- Name: detections_p20260817_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260817_object_id_ts_idx;


--
-- Name: detections_p20260817_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260817_pkey;


--
-- Name: detections_p20260817_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260817_ts_idx;


--
-- Name: detections_p20260818_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260818_camera_id_idx;


--
-- Name: detections_p20260818_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260818_display_id_ts_idx;


--
-- Name: detections_p20260818_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260818_geom_idx;


--
-- Name: detections_p20260818_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260818_object_id_ts_idx;


--
-- Name: detections_p20260818_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260818_pkey;


--
-- Name: detections_p20260818_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260818_ts_idx;


--
-- Name: detections_p20260819_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260819_camera_id_idx;


--
-- Name: detections_p20260819_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260819_display_id_ts_idx;


--
-- Name: detections_p20260819_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260819_geom_idx;


--
-- Name: detections_p20260819_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260819_object_id_ts_idx;


--
-- Name: detections_p20260819_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260819_pkey;


--
-- Name: detections_p20260819_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260819_ts_idx;


--
-- Name: detections_p20260820_camera_id_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_camera ATTACH PARTITION public.detections_p20260820_camera_id_idx;


--
-- Name: detections_p20260820_display_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_display_ts ATTACH PARTITION public.detections_p20260820_display_id_ts_idx;


--
-- Name: detections_p20260820_geom_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_geom ATTACH PARTITION public.detections_p20260820_geom_idx;


--
-- Name: detections_p20260820_object_id_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_objid ATTACH PARTITION public.detections_p20260820_object_id_ts_idx;


--
-- Name: detections_p20260820_pkey; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.detections_pkey ATTACH PARTITION public.detections_p20260820_pkey;


--
-- Name: detections_p20260820_ts_idx; Type: INDEX ATTACH; Schema: public; Owner: -
--

ALTER INDEX public.idx_detections_ts ATTACH PARTITION public.detections_p20260820_ts_idx;


--
-- Name: alerts alerts_incident_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.alerts
    ADD CONSTRAINT alerts_incident_id_fkey FOREIGN KEY (incident_id) REFERENCES public.incidents(incident_id);


--
-- Name: button_event button_event_zone_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.button_event
    ADD CONSTRAINT button_event_zone_id_fkey FOREIGN KEY (zone_id) REFERENCES public.fire_zone(zone_id);


--
-- Name: camera_credentials camera_credentials_camera_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.camera_credentials
    ADD CONSTRAINT camera_credentials_camera_id_fkey FOREIGN KEY (camera_id) REFERENCES public.cameras(camera_id);


--
-- Name: congestion_prediction congestion_prediction_zone_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.congestion_prediction
    ADD CONSTRAINT congestion_prediction_zone_id_fkey FOREIGN KEY (zone_id) REFERENCES public.zones(zone_id);


--
-- Name: fire_event fire_event_cause_channel_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_event
    ADD CONSTRAINT fire_event_cause_channel_id_fkey FOREIGN KEY (cause_channel_id) REFERENCES public.sensor_channel(channel_id);


--
-- Name: fire_event_command fire_event_command_command_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_event_command
    ADD CONSTRAINT fire_event_command_command_id_fkey FOREIGN KEY (command_id) REFERENCES public.actuator_command(command_id);


--
-- Name: fire_event_command fire_event_command_event_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_event_command
    ADD CONSTRAINT fire_event_command_event_id_fkey FOREIGN KEY (event_id) REFERENCES public.fire_event(event_id) ON DELETE CASCADE;


--
-- Name: fire_event fire_event_zone_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.fire_event
    ADD CONSTRAINT fire_event_zone_id_fkey FOREIGN KEY (zone_id) REFERENCES public.fire_zone(zone_id);


--
-- Name: incidents incidents_zone_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.incidents
    ADD CONSTRAINT incidents_zone_id_fkey FOREIGN KEY (zone_id) REFERENCES public.zones(zone_id);


--
-- Name: manual_command manual_command_command_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.manual_command
    ADD CONSTRAINT manual_command_command_id_fkey FOREIGN KEY (command_id) REFERENCES public.actuator_command(command_id);


--
-- Name: manual_command manual_command_zone_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.manual_command
    ADD CONSTRAINT manual_command_zone_id_fkey FOREIGN KEY (zone_id) REFERENCES public.fire_zone(zone_id);


--
-- Name: sensor_reading sensor_reading_zone_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.sensor_reading
    ADD CONSTRAINT sensor_reading_zone_id_fkey FOREIGN KEY (zone_id) REFERENCES public.fire_zone(zone_id);


--
-- Name: sensor_value sensor_value_channel_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.sensor_value
    ADD CONSTRAINT sensor_value_channel_id_fkey FOREIGN KEY (channel_id) REFERENCES public.sensor_channel(channel_id);


--
-- Name: sensor_value sensor_value_reading_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.sensor_value
    ADD CONSTRAINT sensor_value_reading_id_fkey FOREIGN KEY (reading_id) REFERENCES public.sensor_reading(reading_id) ON DELETE CASCADE;


--
-- Name: vms_session vms_session_user_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.vms_session
    ADD CONSTRAINT vms_session_user_id_fkey FOREIGN KEY (user_id) REFERENCES public.vms_user(user_id) ON DELETE CASCADE;


--
-- Name: zone_geometry_history zone_geometry_history_zone_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.zone_geometry_history
    ADD CONSTRAINT zone_geometry_history_zone_id_fkey FOREIGN KEY (zone_id) REFERENCES public.zones(zone_id);


--
-- Name: zone_thresholds zone_thresholds_zone_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.zone_thresholds
    ADD CONSTRAINT zone_thresholds_zone_id_fkey FOREIGN KEY (zone_id) REFERENCES public.zones(zone_id);


--
-- Name: zones zones_camera_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: -
--

ALTER TABLE ONLY public.zones
    ADD CONSTRAINT zones_camera_id_fkey FOREIGN KEY (camera_id) REFERENCES public.cameras(camera_id);


--
-- Name: SCHEMA public; Type: ACL; Schema: -; Owner: -
--

GRANT USAGE ON SCHEMA public TO guardx_writer;
GRANT USAGE ON SCHEMA public TO guardx_reader;
GRANT ALL ON SCHEMA public TO juan;


--
-- Name: TABLE actuator_command; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.actuator_command TO guardx_writer;
GRANT SELECT ON TABLE public.actuator_command TO guardx_reader;


--
-- Name: TABLE alerts; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.alerts TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.alerts TO guardx_writer;
GRANT SELECT ON TABLE public.alerts TO guardx_reader;
GRANT SELECT ON TABLE public.alerts TO guardx_admin;


--
-- Name: SEQUENCE alerts_alert_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.alerts_alert_id_seq TO guardx_writer;
GRANT SELECT,USAGE ON SEQUENCE public.alerts_alert_id_seq TO juan;


--
-- Name: TABLE button_event; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.button_event TO guardx_writer;
GRANT SELECT ON TABLE public.button_event TO guardx_reader;


--
-- Name: SEQUENCE button_event_button_event_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.button_event_button_event_id_seq TO guardx_writer;


--
-- Name: TABLE camera_credentials; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.camera_credentials TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.camera_credentials TO guardx_writer;
GRANT SELECT ON TABLE public.camera_credentials TO guardx_admin;


--
-- Name: TABLE cameras; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.cameras TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.cameras TO guardx_writer;
GRANT SELECT ON TABLE public.cameras TO guardx_reader;
GRANT SELECT ON TABLE public.cameras TO guardx_admin;


--
-- Name: SEQUENCE cameras_camera_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.cameras_camera_id_seq TO guardx_writer;
GRANT SELECT,USAGE ON SEQUENCE public.cameras_camera_id_seq TO juan;


--
-- Name: TABLE congestion_prediction; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.congestion_prediction TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.congestion_prediction TO guardx_writer;
GRANT SELECT ON TABLE public.congestion_prediction TO guardx_reader;
GRANT SELECT ON TABLE public.congestion_prediction TO guardx_admin;


--
-- Name: SEQUENCE congestion_prediction_prediction_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.congestion_prediction_prediction_id_seq TO guardx_writer;
GRANT SELECT,USAGE ON SEQUENCE public.congestion_prediction_prediction_id_seq TO juan;


--
-- Name: TABLE detections; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.detections TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.detections TO guardx_writer;
GRANT SELECT ON TABLE public.detections TO guardx_reader;
GRANT SELECT ON TABLE public.detections TO guardx_admin;


--
-- Name: SEQUENCE detections_detection_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.detections_detection_id_seq TO guardx_writer;
GRANT SELECT,USAGE ON SEQUENCE public.detections_detection_id_seq TO juan;


--
-- Name: TABLE detections_p20260730; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260730 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260730 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260730 TO juan;
GRANT SELECT ON TABLE public.detections_p20260730 TO guardx_admin;


--
-- Name: TABLE detections_p20260731; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260731 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260731 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260731 TO juan;
GRANT SELECT ON TABLE public.detections_p20260731 TO guardx_admin;


--
-- Name: TABLE detections_p20260801; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260801 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260801 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260801 TO juan;
GRANT SELECT ON TABLE public.detections_p20260801 TO guardx_admin;


--
-- Name: TABLE detections_p20260802; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260802 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260802 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260802 TO juan;
GRANT SELECT ON TABLE public.detections_p20260802 TO guardx_admin;


--
-- Name: TABLE detections_p20260803; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260803 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260803 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260803 TO juan;
GRANT SELECT ON TABLE public.detections_p20260803 TO guardx_admin;


--
-- Name: TABLE detections_p20260804; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260804 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260804 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260804 TO juan;


--
-- Name: TABLE detections_p20260805; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260805 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260805 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260805 TO juan;


--
-- Name: TABLE detections_p20260806; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260806 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260806 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260806 TO juan;


--
-- Name: TABLE detections_p20260807; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260807 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260807 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260807 TO juan;


--
-- Name: TABLE detections_p20260808; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260808 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260808 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260808 TO juan;


--
-- Name: TABLE detections_p20260809; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260809 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260809 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260809 TO juan;


--
-- Name: TABLE detections_p20260810; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260810 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260810 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260810 TO juan;


--
-- Name: TABLE detections_p20260811; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260811 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260811 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260811 TO juan;


--
-- Name: TABLE detections_p20260812; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260812 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260812 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260812 TO juan;


--
-- Name: TABLE detections_p20260813; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260813 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260813 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260813 TO juan;


--
-- Name: TABLE detections_p20260814; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260814 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260814 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260814 TO juan;


--
-- Name: TABLE detections_p20260815; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260815 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260815 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260815 TO juan;


--
-- Name: TABLE detections_p20260816; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260816 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260816 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260816 TO juan;


--
-- Name: TABLE detections_p20260817; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260817 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260817 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260817 TO juan;


--
-- Name: TABLE detections_p20260818; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260818 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260818 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260818 TO juan;


--
-- Name: TABLE detections_p20260819; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260819 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260819 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260819 TO juan;


--
-- Name: TABLE detections_p20260820; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.detections_p20260820 TO guardx_writer;
GRANT SELECT ON TABLE public.detections_p20260820 TO guardx_reader;
GRANT ALL ON TABLE public.detections_p20260820 TO juan;


--
-- Name: TABLE endpoints; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.endpoints TO guardx_writer;
GRANT SELECT ON TABLE public.endpoints TO guardx_reader;
GRANT SELECT ON TABLE public.endpoints TO juan;


--
-- Name: TABLE faces; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.faces TO guardx_writer;
GRANT SELECT ON TABLE public.faces TO guardx_reader;
GRANT ALL ON TABLE public.faces TO juan;
GRANT SELECT ON TABLE public.faces TO guardx_admin;


--
-- Name: SEQUENCE faces_face_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.faces_face_id_seq TO guardx_writer;
GRANT SELECT,USAGE ON SEQUENCE public.faces_face_id_seq TO juan;


--
-- Name: TABLE fire_event; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.fire_event TO guardx_writer;
GRANT SELECT ON TABLE public.fire_event TO guardx_reader;


--
-- Name: TABLE fire_event_command; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.fire_event_command TO guardx_writer;
GRANT SELECT ON TABLE public.fire_event_command TO guardx_reader;


--
-- Name: SEQUENCE fire_event_event_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.fire_event_event_id_seq TO guardx_writer;


--
-- Name: TABLE fire_threshold; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.fire_threshold TO guardx_writer;
GRANT SELECT ON TABLE public.fire_threshold TO guardx_reader;


--
-- Name: SEQUENCE fire_threshold_threshold_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.fire_threshold_threshold_id_seq TO guardx_writer;


--
-- Name: TABLE fire_zone; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.fire_zone TO guardx_writer;
GRANT SELECT ON TABLE public.fire_zone TO guardx_reader;


--
-- Name: TABLE geography_columns; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.geography_columns TO guardx_writer;
GRANT SELECT ON TABLE public.geography_columns TO guardx_reader;
GRANT SELECT,INSERT,UPDATE ON TABLE public.geography_columns TO juan;
GRANT SELECT ON TABLE public.geography_columns TO guardx_admin;


--
-- Name: TABLE geometry_columns; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.geometry_columns TO guardx_writer;
GRANT SELECT ON TABLE public.geometry_columns TO guardx_reader;
GRANT SELECT,INSERT,UPDATE ON TABLE public.geometry_columns TO juan;
GRANT SELECT ON TABLE public.geometry_columns TO guardx_admin;


--
-- Name: TABLE incidents; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.incidents TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.incidents TO guardx_writer;
GRANT SELECT ON TABLE public.incidents TO guardx_reader;
GRANT SELECT ON TABLE public.incidents TO guardx_admin;


--
-- Name: SEQUENCE incidents_incident_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.incidents_incident_id_seq TO guardx_writer;
GRANT SELECT,USAGE ON SEQUENCE public.incidents_incident_id_seq TO juan;


--
-- Name: TABLE line_flow; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.line_flow TO guardx_writer;
GRANT SELECT ON TABLE public.line_flow TO guardx_reader;
GRANT ALL ON TABLE public.line_flow TO juan;
GRANT SELECT ON TABLE public.line_flow TO guardx_admin;


--
-- Name: TABLE manual_command; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.manual_command TO guardx_writer;
GRANT SELECT ON TABLE public.manual_command TO guardx_reader;


--
-- Name: SEQUENCE manual_command_manual_command_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.manual_command_manual_command_id_seq TO guardx_writer;


--
-- Name: TABLE trajectory_segments; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.trajectory_segments TO guardx_writer;
GRANT SELECT ON TABLE public.trajectory_segments TO guardx_reader;
GRANT SELECT ON TABLE public.trajectory_segments TO juan;


--
-- Name: TABLE reliable_trajectory_segments; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT ON TABLE public.reliable_trajectory_segments TO guardx_reader;
GRANT SELECT ON TABLE public.reliable_trajectory_segments TO guardx_writer;
GRANT SELECT ON TABLE public.reliable_trajectory_segments TO juan;


--
-- Name: TABLE schema_migrations; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.schema_migrations TO guardx_writer;
GRANT SELECT ON TABLE public.schema_migrations TO guardx_reader;
GRANT ALL ON TABLE public.schema_migrations TO juan;


--
-- Name: TABLE season_threshold; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.season_threshold TO guardx_writer;
GRANT SELECT ON TABLE public.season_threshold TO guardx_reader;
GRANT SELECT ON TABLE public.season_threshold TO juan;


--
-- Name: TABLE sensor_channel; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.sensor_channel TO guardx_writer;
GRANT SELECT ON TABLE public.sensor_channel TO guardx_reader;


--
-- Name: TABLE sensor_reading; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.sensor_reading TO guardx_writer;
GRANT SELECT ON TABLE public.sensor_reading TO guardx_reader;


--
-- Name: SEQUENCE sensor_reading_reading_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.sensor_reading_reading_id_seq TO guardx_writer;


--
-- Name: TABLE sensor_value; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.sensor_value TO guardx_writer;
GRANT SELECT ON TABLE public.sensor_value TO guardx_reader;


--
-- Name: TABLE site_config; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.site_config TO guardx_writer;
GRANT SELECT ON TABLE public.site_config TO guardx_reader;


--
-- Name: TABLE spatial_ref_sys; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.spatial_ref_sys TO guardx_writer;
GRANT SELECT ON TABLE public.spatial_ref_sys TO guardx_reader;
GRANT SELECT,INSERT,UPDATE ON TABLE public.spatial_ref_sys TO juan;
GRANT SELECT ON TABLE public.spatial_ref_sys TO guardx_admin;


--
-- Name: TABLE track_path; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.track_path TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.track_path TO guardx_writer;
GRANT SELECT ON TABLE public.track_path TO guardx_reader;
GRANT SELECT ON TABLE public.track_path TO guardx_admin;


--
-- Name: TABLE tracks; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.tracks TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.tracks TO guardx_writer;
GRANT SELECT ON TABLE public.tracks TO guardx_reader;
GRANT SELECT ON TABLE public.tracks TO guardx_admin;


--
-- Name: SEQUENCE tracks_track_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.tracks_track_id_seq TO guardx_writer;
GRANT SELECT,USAGE ON SEQUENCE public.tracks_track_id_seq TO juan;


--
-- Name: TABLE trajectory_zone_dwell_summary; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT ON TABLE public.trajectory_zone_dwell_summary TO guardx_reader;
GRANT SELECT ON TABLE public.trajectory_zone_dwell_summary TO guardx_writer;
GRANT SELECT ON TABLE public.trajectory_zone_dwell_summary TO juan;


--
-- Name: TABLE trajectory_zone_transition_summary; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT ON TABLE public.trajectory_zone_transition_summary TO guardx_reader;
GRANT SELECT ON TABLE public.trajectory_zone_transition_summary TO guardx_writer;
GRANT SELECT ON TABLE public.trajectory_zone_transition_summary TO juan;


--
-- Name: TABLE vms_session; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,DELETE,UPDATE ON TABLE public.vms_session TO guardx_writer;


--
-- Name: TABLE vms_user; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,INSERT,UPDATE ON TABLE public.vms_user TO guardx_writer;


--
-- Name: SEQUENCE vms_user_user_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.vms_user_user_id_seq TO guardx_writer;


--
-- Name: TABLE zone_geometry_history; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.zone_geometry_history TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.zone_geometry_history TO guardx_writer;
GRANT SELECT ON TABLE public.zone_geometry_history TO guardx_reader;
GRANT SELECT ON TABLE public.zone_geometry_history TO guardx_admin;


--
-- Name: SEQUENCE zone_geometry_history_geom_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.zone_geometry_history_geom_id_seq TO guardx_writer;
GRANT SELECT,USAGE ON SEQUENCE public.zone_geometry_history_geom_id_seq TO juan;


--
-- Name: TABLE zone_occupancy; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.zone_occupancy TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.zone_occupancy TO guardx_writer;
GRANT SELECT ON TABLE public.zone_occupancy TO guardx_reader;
GRANT SELECT ON TABLE public.zone_occupancy TO guardx_admin;


--
-- Name: TABLE zone_thresholds; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.zone_thresholds TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.zone_thresholds TO guardx_writer;
GRANT SELECT ON TABLE public.zone_thresholds TO guardx_reader;
GRANT SELECT ON TABLE public.zone_thresholds TO guardx_admin;


--
-- Name: TABLE zones; Type: ACL; Schema: public; Owner: -
--

GRANT ALL ON TABLE public.zones TO juan;
GRANT SELECT,INSERT,UPDATE ON TABLE public.zones TO guardx_writer;
GRANT SELECT ON TABLE public.zones TO guardx_reader;
GRANT SELECT ON TABLE public.zones TO guardx_admin;


--
-- Name: SEQUENCE zones_zone_id_seq; Type: ACL; Schema: public; Owner: -
--

GRANT SELECT,USAGE ON SEQUENCE public.zones_zone_id_seq TO guardx_writer;
GRANT SELECT,USAGE ON SEQUENCE public.zones_zone_id_seq TO juan;


--
-- Name: DEFAULT PRIVILEGES FOR SEQUENCES; Type: DEFAULT ACL; Schema: public; Owner: -
--

ALTER DEFAULT PRIVILEGES FOR ROLE guardx_admin IN SCHEMA public GRANT SELECT,USAGE ON SEQUENCES TO guardx_writer;


--
-- Name: DEFAULT PRIVILEGES FOR TABLES; Type: DEFAULT ACL; Schema: public; Owner: -
--

ALTER DEFAULT PRIVILEGES FOR ROLE guardx_admin IN SCHEMA public GRANT SELECT,INSERT,UPDATE ON TABLES TO guardx_writer;
ALTER DEFAULT PRIVILEGES FOR ROLE guardx_admin IN SCHEMA public GRANT SELECT ON TABLES TO guardx_reader;


--
-- Name: DEFAULT PRIVILEGES FOR TABLES; Type: DEFAULT ACL; Schema: public; Owner: -
--

ALTER DEFAULT PRIVILEGES FOR ROLE postgres IN SCHEMA public GRANT SELECT,INSERT,UPDATE ON TABLES TO guardx_writer;
ALTER DEFAULT PRIVILEGES FOR ROLE postgres IN SCHEMA public GRANT SELECT ON TABLES TO guardx_reader;
ALTER DEFAULT PRIVILEGES FOR ROLE postgres IN SCHEMA public GRANT ALL ON TABLES TO juan;


--
-- PostgreSQL database dump complete
--

\unrestrict shWqunO8HLXNaE3KS7Y2TjUXuw2Kbp1RsRnk1nFGw5dnrD9xk0m2mv3pZZeTPJN

