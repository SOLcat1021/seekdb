// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// A file-format decoder for seekdb's native SQL file table. No SQL execution
// happens here: filtering, aggregation, sorting and joins stay in seekdb.

use std::fs::File;
use std::ptr;

use arrow_array::{
    Array, BinaryArray, BooleanArray, Date32Array, Date64Array, Float32Array, Float64Array,
    Int16Array, Int32Array, Int64Array, Int8Array, LargeBinaryArray, LargeStringArray, RecordBatch,
    StringArray, TimestampMicrosecondArray, TimestampMillisecondArray, TimestampNanosecondArray,
    TimestampSecondArray, UInt16Array, UInt32Array, UInt8Array,
};
use arrow_schema::DataType;
use chrono::{DateTime, Duration, NaiveDate, Utc};
use parquet::arrow::arrow_reader::{ParquetRecordBatchReader, ParquetRecordBatchReaderBuilder};

const MAX_COLUMNS: usize = 4096;
const BATCH_SIZE: usize = 1024;
const MAX_CELL_BYTES: usize = 16 * 1024 * 1024;

pub struct FileSqlParquetReader {
    reader: ParquetRecordBatchReader,
    columns: Vec<Column>,
    batch: Option<RecordBatch>,
    row: usize,
    values: Vec<Vec<u8>>,
    nulls: Vec<bool>,
    error: String,
}

struct Column {
    name: String,
    kind: i32,
    nullable: bool,
}

fn column_kind(data_type: &DataType) -> Result<i32, String> {
    match data_type {
        DataType::Boolean => Ok(1),
        DataType::Int8
        | DataType::Int16
        | DataType::Int32
        | DataType::Int64
        | DataType::UInt8
        | DataType::UInt16
        | DataType::UInt32 => Ok(2),
        DataType::Float32 | DataType::Float64 => Ok(3),
        DataType::Date32 | DataType::Date64 => Ok(4),
        DataType::Timestamp(_, _) => Ok(5),
        DataType::Utf8 | DataType::LargeUtf8 | DataType::Binary | DataType::LargeBinary => Ok(6),
        other => Err(format!("unsupported Parquet column type: {other:?}")),
    }
}

fn format_date(days: i64) -> Result<Vec<u8>, String> {
    let epoch = NaiveDate::from_ymd_opt(1970, 1, 1).ok_or("invalid date epoch")?;
    let value = epoch
        .checked_add_signed(Duration::days(days))
        .ok_or("Parquet date out of range")?;
    Ok(value.format("%Y-%m-%d").to_string().into_bytes())
}

fn format_timestamp(seconds: i64, nanos: u32) -> Result<Vec<u8>, String> {
    let value =
        DateTime::<Utc>::from_timestamp(seconds, nanos).ok_or("Parquet timestamp out of range")?;
    Ok(value
        .format("%Y-%m-%d %H:%M:%S%.f")
        .to_string()
        .into_bytes())
}

fn cell_value(array: &dyn Array, row: usize) -> Result<Vec<u8>, String> {
    macro_rules! value {
        ($array:ty) => {{
            array
                .as_any()
                .downcast_ref::<$array>()
                .ok_or("Parquet array type mismatch")?
                .value(row)
        }};
    }
    let bytes = match array.data_type() {
        DataType::Boolean => value!(BooleanArray).to_string().into_bytes(),
        DataType::Int8 => value!(Int8Array).to_string().into_bytes(),
        DataType::Int16 => value!(Int16Array).to_string().into_bytes(),
        DataType::Int32 => value!(Int32Array).to_string().into_bytes(),
        DataType::Int64 => value!(Int64Array).to_string().into_bytes(),
        DataType::UInt8 => value!(UInt8Array).to_string().into_bytes(),
        DataType::UInt16 => value!(UInt16Array).to_string().into_bytes(),
        DataType::UInt32 => value!(UInt32Array).to_string().into_bytes(),
        DataType::Float32 => value!(Float32Array).to_string().into_bytes(),
        DataType::Float64 => value!(Float64Array).to_string().into_bytes(),
        DataType::Utf8 => value!(StringArray).as_bytes().to_vec(),
        DataType::LargeUtf8 => value!(LargeStringArray).as_bytes().to_vec(),
        DataType::Binary => value!(BinaryArray).to_vec(),
        DataType::LargeBinary => value!(LargeBinaryArray).to_vec(),
        DataType::Date32 => format_date(i64::from(value!(Date32Array)))?,
        DataType::Date64 => format_date(value!(Date64Array).div_euclid(86_400_000))?,
        DataType::Timestamp(_, _) => match array.data_type() {
            DataType::Timestamp(arrow_schema::TimeUnit::Second, _) => {
                format_timestamp(value!(TimestampSecondArray), 0)?
            }
            DataType::Timestamp(arrow_schema::TimeUnit::Millisecond, _) => {
                let v = value!(TimestampMillisecondArray);
                format_timestamp(
                    v.div_euclid(1_000),
                    (v.rem_euclid(1_000) * 1_000_000) as u32,
                )?
            }
            DataType::Timestamp(arrow_schema::TimeUnit::Microsecond, _) => {
                let v = value!(TimestampMicrosecondArray);
                format_timestamp(
                    v.div_euclid(1_000_000),
                    (v.rem_euclid(1_000_000) * 1_000) as u32,
                )?
            }
            DataType::Timestamp(arrow_schema::TimeUnit::Nanosecond, _) => {
                let v = value!(TimestampNanosecondArray);
                format_timestamp(
                    v.div_euclid(1_000_000_000),
                    v.rem_euclid(1_000_000_000) as u32,
                )?
            }
            _ => return Err("unsupported Parquet timestamp unit".into()),
        },
        other => return Err(format!("unsupported Parquet column type: {other:?}")),
    };
    if bytes.len() > MAX_CELL_BYTES {
        return Err("Parquet cell exceeds 16 MiB limit".into());
    }
    Ok(bytes)
}

fn open_reader(path: &str) -> Result<FileSqlParquetReader, String> {
    let file = File::open(path).map_err(|e| format!("cannot open Parquet file: {e}"))?;
    let builder = ParquetRecordBatchReaderBuilder::try_new(file)
        .map_err(|e| format!("invalid Parquet footer: {e}"))?;
    let schema = builder.schema();
    if schema.fields().is_empty() || schema.fields().len() > MAX_COLUMNS {
        return Err("Parquet column count is outside supported range".into());
    }
    let mut columns = Vec::with_capacity(schema.fields().len());
    for field in schema.fields() {
        columns.push(Column {
            name: field.name().clone(),
            kind: column_kind(field.data_type())?,
            nullable: field.is_nullable(),
        });
    }
    let reader = builder
        .with_batch_size(BATCH_SIZE)
        .build()
        .map_err(|e| format!("cannot start Parquet scan: {e}"))?;
    Ok(FileSqlParquetReader {
        reader,
        values: vec![Vec::new(); columns.len()],
        nulls: vec![true; columns.len()],
        columns,
        batch: None,
        row: 0,
        error: String::new(),
    })
}

fn copy_text(text: &str, output: *mut i8, capacity: usize) -> usize {
    if !output.is_null() && capacity > 0 {
        let count = text.len().min(capacity - 1);
        // SAFETY: The caller promises an output buffer of capacity bytes.
        unsafe {
            ptr::copy_nonoverlapping(text.as_ptr(), output.cast::<u8>(), count);
            *output.add(count) = 0;
        }
    }
    text.len()
}

#[no_mangle]
pub extern "C" fn file_sql_parquet_open(
    path: *const u8,
    path_len: usize,
    error: *mut i8,
    error_capacity: usize,
) -> *mut FileSqlParquetReader {
    if path.is_null() || path_len == 0 || path_len > 4096 {
        copy_text("invalid Parquet path", error, error_capacity);
        return ptr::null_mut();
    }
    // SAFETY: path points to path_len bytes during this call.
    let bytes = unsafe { std::slice::from_raw_parts(path, path_len) };
    let path = match std::str::from_utf8(bytes) {
        Ok(path) if !path.contains('\0') => path,
        _ => {
            copy_text(
                "Parquet path must be UTF-8 without NUL",
                error,
                error_capacity,
            );
            return ptr::null_mut();
        }
    };
    match open_reader(path) {
        Ok(reader) => Box::into_raw(Box::new(reader)),
        Err(message) => {
            copy_text(&message, error, error_capacity);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub extern "C" fn file_sql_parquet_close(reader: *mut FileSqlParquetReader) {
    if !reader.is_null() {
        // SAFETY: The caller owns the pointer from file_sql_parquet_open.
        unsafe {
            drop(Box::from_raw(reader));
        }
    }
}

#[no_mangle]
pub extern "C" fn file_sql_parquet_column_count(reader: *const FileSqlParquetReader) -> usize {
    // SAFETY: Null is checked and the caller owns a live reader.
    unsafe { reader.as_ref().map_or(0, |r| r.columns.len()) }
}

#[no_mangle]
pub extern "C" fn file_sql_parquet_column_name(
    reader: *const FileSqlParquetReader,
    column: usize,
    output: *mut i8,
    capacity: usize,
) -> usize {
    // SAFETY: Null is checked and the caller owns a live reader.
    unsafe { reader.as_ref().and_then(|r| r.columns.get(column)) }
        .map_or(0, |c| copy_text(&c.name, output, capacity))
}

#[no_mangle]
pub extern "C" fn file_sql_parquet_column_type(
    reader: *const FileSqlParquetReader,
    column: usize,
) -> i32 {
    // SAFETY: Null is checked and the caller owns a live reader.
    unsafe { reader.as_ref().and_then(|r| r.columns.get(column)) }.map_or(-1, |c| c.kind)
}

#[no_mangle]
pub extern "C" fn file_sql_parquet_column_nullable(
    reader: *const FileSqlParquetReader,
    column: usize,
) -> i32 {
    // SAFETY: Null is checked and the caller owns a live reader.
    unsafe { reader.as_ref().and_then(|r| r.columns.get(column)) }
        .map_or(-1, |c| i32::from(c.nullable))
}

#[no_mangle]
pub extern "C" fn file_sql_parquet_next(reader: *mut FileSqlParquetReader) -> i32 {
    // SAFETY: Null is checked and the caller owns a live reader.
    let Some(reader) = (unsafe { reader.as_mut() }) else {
        return -1;
    };
    reader.error.clear();
    loop {
        if let Some(batch) = &reader.batch {
            if reader.row < batch.num_rows() {
                let row = reader.row;
                let mut row_bytes = 0usize;
                for column in 0..reader.columns.len() {
                    let array = batch.column(column);
                    reader.nulls[column] = array.is_null(row);
                    if !reader.nulls[column] {
                        match cell_value(array.as_ref(), row) {
                            Ok(value) => {
                                if value.len() > MAX_CELL_BYTES - row_bytes {
                                    reader.error = "Parquet row exceeds 16 MiB limit".into();
                                    return -1;
                                }
                                row_bytes += value.len();
                                reader.values[column] = value;
                            }
                            Err(message) => {
                                reader.error = message;
                                return -1;
                            }
                        }
                    } else {
                        reader.values[column].clear();
                    }
                }
                reader.row += 1;
                return 1;
            }
        }
        match reader.reader.next() {
            Some(Ok(batch)) => {
                reader.batch = Some(batch);
                reader.row = 0;
            }
            Some(Err(e)) => {
                reader.error = format!("Parquet page decode failed: {e}");
                return -1;
            }
            None => return 0,
        }
    }
}

#[no_mangle]
pub extern "C" fn file_sql_parquet_cell(
    reader: *const FileSqlParquetReader,
    column: usize,
    data: *mut *const u8,
    length: *mut usize,
    is_null: *mut i32,
) -> i32 {
    if data.is_null() || length.is_null() || is_null.is_null() {
        return -1;
    }
    // SAFETY: Null is checked and the caller owns a live reader.
    let Some(reader) = (unsafe { reader.as_ref() }) else {
        return -1;
    };
    let Some(value) = reader.values.get(column) else {
        return -1;
    };
    // SAFETY: Callers supply valid output pointers; bytes live until next/close.
    unsafe {
        *data = value.as_ptr();
        *length = value.len();
        *is_null = i32::from(reader.nulls[column]);
    }
    0
}

#[no_mangle]
pub extern "C" fn file_sql_parquet_error(
    reader: *const FileSqlParquetReader,
    output: *mut i8,
    capacity: usize,
) -> usize {
    // SAFETY: Null is checked and the caller owns a live reader.
    unsafe { reader.as_ref() }.map_or(0, |r| copy_text(&r.error, output, capacity))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    use arrow_array::Int32Array;
    use arrow_schema::{Field, Schema};
    use parquet::arrow::ArrowWriter;
    use parquet::basic::Compression;
    use parquet::file::properties::WriterProperties;

    #[test]
    fn snappy_nullable_multi_row_group() {
        let path = std::env::temp_dir().join(format!(
            "seekdb-file-sql-parquet-{}.parquet",
            std::process::id()
        ));
        let schema = Arc::new(Schema::new(vec![Field::new("c1", DataType::Int32, true)]));
        let properties = WriterProperties::builder()
            .set_compression(Compression::SNAPPY)
            .build();
        let file = File::create(&path).unwrap();
        let mut writer = ArrowWriter::try_new(file, schema.clone(), Some(properties)).unwrap();
        for rows in [vec![Some(0), None, Some(2)], vec![Some(3), Some(4)]] {
            let batch =
                RecordBatch::try_new(schema.clone(), vec![Arc::new(Int32Array::from(rows))])
                    .unwrap();
            writer.write(&batch).unwrap();
            writer.flush().unwrap();
        }
        writer.close().unwrap();
        let mut reader = open_reader(path.to_str().unwrap()).unwrap();
        assert_eq!(reader.columns.len(), 1);
        assert_eq!(reader.columns[0].kind, 2);
        assert!(reader.columns[0].nullable);
        let mut observed = Vec::new();
        while file_sql_parquet_next(&mut reader) == 1 {
            observed.push(if reader.nulls[0] {
                None
            } else {
                Some(String::from_utf8(reader.values[0].clone()).unwrap())
            });
        }
        assert_eq!(
            observed,
            [
                Some("0".into()),
                None,
                Some("2".into()),
                Some("3".into()),
                Some("4".into())
            ]
        );
        std::fs::remove_file(path).unwrap();
    }
}
